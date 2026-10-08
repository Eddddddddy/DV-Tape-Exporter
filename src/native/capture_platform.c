/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#else
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#endif

#include "capture_platform.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

static void capture_set_windows_errno(DWORD error)
{
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
        errno = ENOENT;
        break;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        errno = EEXIST;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        errno = EACCES;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        errno = ENOMEM;
        break;
    case ERROR_INVALID_HANDLE:
        errno = EBADF;
        break;
    case ERROR_NO_UNICODE_TRANSLATION:
        errno = EILSEQ;
        break;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_NAME:
        errno = EINVAL;
        break;
    case ERROR_DISK_FULL:
        errno = ENOSPC;
        break;
    default:
        errno = EIO;
        break;
    }
}

static wchar_t *capture_path_wide(const char *path)
{
    int count;
    wchar_t *wide;

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
    count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                              NULL, 0);
    if (count == 0) {
        capture_set_windows_errno(GetLastError());
        return NULL;
    }
    wide = (wchar_t *)malloc((size_t)count * sizeof(*wide));
    if (wide == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            wide, count) == 0) {
        DWORD error = GetLastError();
        free(wide);
        capture_set_windows_errno(error);
        return NULL;
    }
    return wide;
}

FILE *capture_open_new_utf8(const char *path)
{
    wchar_t *wide = capture_path_wide(path);
    HANDLE handle;
    int descriptor;
    FILE *stream;
    int saved_errno;

    if (wide == NULL)
        return NULL;
    handle = CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                         CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        capture_set_windows_errno(GetLastError());
        return NULL;
    }
    descriptor = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_WRONLY);
    if (descriptor < 0) {
        saved_errno = errno;
        CloseHandle(handle);
        errno = saved_errno;
        return NULL;
    }
    stream = _fdopen(descriptor, "wb");
    if (stream == NULL) {
        saved_errno = errno;
        _close(descriptor); /* The CRT owns handle after _open_osfhandle. */
        errno = saved_errno;
    }
    return stream;
}

int64_t capture_file_size_utf8(const char *path)
{
    wchar_t *wide = capture_path_wide(path);
    struct _stat64 info;
    int result;
    int saved_errno;

    if (wide == NULL)
        return -1;
    result = _wstat64(wide, &info);
    saved_errno = errno;
    free(wide);
    if (result != 0) {
        errno = saved_errno;
        return -1;
    }
    return (int64_t)info.st_size;
}

int capture_path_exists_utf8(const char *path)
{
    wchar_t *wide = capture_path_wide(path);
    struct _stat64 info;
    int result;
    int saved_errno;

    if (wide == NULL)
        return -1;
    result = _wstat64(wide, &info);
    saved_errno = errno;
    free(wide);
    if (result == 0)
        return 1;
    errno = saved_errno;
    return saved_errno == ENOENT || saved_errno == ENOTDIR ? 0 : -1;
}

int capture_stdin_poll(unsigned char *buffer, size_t capacity, int timeout_ms)
{
    HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD type;
    DWORD error;
    ULONGLONG deadline;

    if (capacity == 0)
        return 0;
    if (buffer == NULL) {
        errno = EINVAL;
        return -2;
    }
    if (handle == NULL || handle == INVALID_HANDLE_VALUE)
        return -1;
    SetLastError(ERROR_SUCCESS);
    type = GetFileType(handle);
    error = GetLastError();
    if (type == FILE_TYPE_CHAR) {
        /* ReadConsole/ReadFile may await a line; never call either here. */
        if (timeout_ms > 0)
            Sleep((DWORD)timeout_ms);
        return 0;
    }
    if (type == FILE_TYPE_UNKNOWN && error != ERROR_SUCCESS) {
        capture_set_windows_errno(error);
        return -2;
    }
    if (type != FILE_TYPE_PIPE && type != FILE_TYPE_DISK) {
        errno = ENOTSUP;
        return -2;
    }
    deadline = GetTickCount64() + (ULONGLONG)(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        DWORD available = (DWORD)(capacity > INT_MAX ? INT_MAX : capacity);
        DWORD received = 0;
        if (type == FILE_TYPE_PIPE) {
            if (!PeekNamedPipe(handle, NULL, 0, NULL, &available, NULL)) {
                error = GetLastError();
                if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED)
                    return -1;
                capture_set_windows_errno(error);
                return -2;
            }
            if (available == 0) {
                ULONGLONG now = GetTickCount64();
                ULONGLONG remaining;
                if (now >= deadline)
                    return 0;
                remaining = deadline - now;
                Sleep((DWORD)(remaining < 10 ? remaining : 10));
                continue;
            }
            if ((size_t)available > capacity)
                available = (DWORD)(capacity > INT_MAX ? INT_MAX : capacity);
            if (available > INT_MAX)
                available = INT_MAX;
        }
        /* The sole reader requests only bytes reported available by the pipe. */
        if (!ReadFile(handle, buffer, available, &received, NULL)) {
            error = GetLastError();
            if (error == ERROR_MORE_DATA && received > 0)
                return (int)received;
            if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF ||
                error == ERROR_PIPE_NOT_CONNECTED)
                return -1;
            capture_set_windows_errno(error);
            return -2;
        }
        return received == 0 ? -1 : (int)received;
    }
}

void capture_sleep_ms(int milliseconds)
{
    if (milliseconds > 0)
        Sleep((DWORD)milliseconds);
}

uint64_t capture_monotonic_ms(void)
{
    return (uint64_t)GetTickCount64();
}

void capture_init_stdio(void)
{
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdin), _O_BINARY);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

int capture_wait_child(intptr_t child, int *status)
{
    (void)child;
    (void)status;
    errno = ENOTSUP;
    return -1;
}

int capture_terminal_size(int *columns, int *rows)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (columns == NULL || rows == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info) &&
        !GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &info)) {
        errno = ENOTTY;
        return -1;
    }
    *columns = (int)(info.srWindow.Right - info.srWindow.Left + 1);
    *rows = (int)(info.srWindow.Bottom - info.srWindow.Top + 1);
    return 0;
}

char **capture_argv_utf8(int argc, wchar_t *const argv[])
{
    char **converted;
    int index;

    if (argc < 0 || argv == NULL || (size_t)argc > SIZE_MAX / sizeof(char *) - 1) {
        errno = EINVAL;
        return NULL;
    }
    converted = (char **)calloc((size_t)argc + 1, sizeof(*converted));
    if (converted == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    for (index = 0; index < argc; ++index) {
        int bytes;
        if (argv[index] == NULL) {
            errno = EINVAL;
            goto failed;
        }
        bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index],
                                    -1, NULL, 0, NULL, NULL);
        if (bytes == 0) {
            capture_set_windows_errno(GetLastError());
            goto failed;
        }
        converted[index] = (char *)malloc((size_t)bytes);
        if (converted[index] == NULL) {
            errno = ENOMEM;
            goto failed;
        }
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1,
                                converted[index], bytes, NULL, NULL) == 0) {
            capture_set_windows_errno(GetLastError());
            goto failed;
        }
    }
    return converted;

failed:
    {
        int saved_errno = errno;
        capture_free_argv_utf8(argc, converted);
        errno = saved_errno;
    }
    return NULL;
}

void capture_free_argv_utf8(int argc, char **argv)
{
    int index;
    if (argv == NULL)
        return;
    for (index = 0; index < argc; ++index)
        free(argv[index]);
    free(argv);
}

#else

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

uint64_t capture_monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return UINT64_MAX;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

FILE *capture_open_new_utf8(const char *path)
{
    int flags = O_WRONLY | O_CREAT | O_EXCL;
    int descriptor;
    FILE *stream;
    int saved_errno;

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    descriptor = open(path, flags, 0666);
    if (descriptor < 0)
        return NULL;
    stream = fdopen(descriptor, "wb");
    if (stream == NULL) {
        saved_errno = errno;
        close(descriptor);
        errno = saved_errno;
    }
    return stream;
}

int64_t capture_file_size_utf8(const char *path)
{
    struct stat info;
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (stat(path, &info) != 0)
        return -1;
    return (int64_t)info.st_size;
}

int capture_path_exists_utf8(const char *path)
{
    struct stat info;
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (stat(path, &info) == 0)
        return 1;
    return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
}

int capture_stdin_poll(unsigned char *buffer, size_t capacity, int timeout_ms)
{
    struct pollfd descriptor;
    uint64_t deadline;
    int wait_ms = timeout_ms > 0 ? timeout_ms : 0;

    if (capacity == 0)
        return 0;
    if (buffer == NULL) {
        errno = EINVAL;
        return -2;
    }
    deadline = capture_monotonic_ms();
    if (deadline == UINT64_MAX)
        return -2;
    deadline += wait_ms;
    descriptor.fd = STDIN_FILENO;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    for (;;) {
        int ready = poll(&descriptor, 1, wait_ms);
        if (ready < 0 && errno == EINTR) {
            uint64_t now = capture_monotonic_ms();
            if (now == UINT64_MAX)
                return -2;
            if (now >= deadline)
                return 0;
            wait_ms = (int)(deadline - now);
            continue;
        }
        if (ready < 0)
            return -2;
        if (ready == 0)
            return 0;
        if (descriptor.revents & (POLLIN | POLLHUP | POLLNVAL)) {
            int flags = fcntl(STDIN_FILENO, F_GETFL);
            ssize_t received;
            int saved_errno;
            int changed;
            if (flags < 0)
                return -2;
            changed = (flags & O_NONBLOCK) == 0;
            if (changed && fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0)
                return -2;
            /*
             * Nonblocking read protects against stale readiness. Darwin reports
             * POLLNVAL for some valid devices (including /dev/null); F_GETFL
             * distinguishes them from invalid descriptors before a safe read.
             */
            received = read(STDIN_FILENO, buffer,
                            capacity > INT_MAX ? (size_t)INT_MAX : capacity);
            saved_errno = errno;
            if (changed && fcntl(STDIN_FILENO, F_SETFL, flags) < 0)
                return -2;
            errno = saved_errno;
            if (received > 0)
                return (int)received;
            if (received == 0)
                return -1;
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                uint64_t now = capture_monotonic_ms();
                if (now == UINT64_MAX)
                    return -2;
                if (now >= deadline)
                    return 0;
                wait_ms = (int)(deadline - now);
                if (descriptor.revents & POLLNVAL) {
                    capture_sleep_ms(wait_ms < 10 ? wait_ms : 10);
                    now = capture_monotonic_ms();
                    if (now == UINT64_MAX)
                        return -2;
                    if (now >= deadline)
                        return 0;
                    wait_ms = (int)(deadline - now);
                }
                continue;
            }
            return -2;
        }
        if (descriptor.revents & POLLERR) {
            errno = EIO;
            return -2;
        }
        return 0;
    }
}

void capture_sleep_ms(int milliseconds)
{
    struct timespec remaining;
    if (milliseconds <= 0)
        return;
    remaining.tv_sec = milliseconds / 1000;
    remaining.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
    }
}

void capture_init_stdio(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

int capture_wait_child(intptr_t child, int *status)
{
    pid_t result;
    if (child <= 0 || status == NULL || (intptr_t)(pid_t)child != child) {
        errno = EINVAL;
        return -1;
    }
    do {
        result = waitpid((pid_t)child, status, 0);
    } while (result < 0 && errno == EINTR);
    return result < 0 ? -1 : 0;
}

int capture_terminal_size(int *columns, int *rows)
{
    struct winsize info;
    if (columns == NULL || rows == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &info) != 0 &&
        ioctl(STDERR_FILENO, TIOCGWINSZ, &info) != 0)
        return -1;
    if (info.ws_col == 0 || info.ws_row == 0) {
        errno = ENOTTY;
        return -1;
    }
    *columns = (int)info.ws_col;
    *rows = (int)info.ws_row;
    return 0;
}

#endif
