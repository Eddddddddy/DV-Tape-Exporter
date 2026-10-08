/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Synthetic helper tests only; no device or media access is performed. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#endif
#include "capture_platform.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

static int expect(int condition, const char *message)
{
    if (condition)
        return 0;
    fprintf(stderr, "FAIL: %s (errno=%d)\n", message, errno);
    return 1;
}

static int remove_utf8(const char *path)
{
#ifdef _WIN32
    wchar_t wide[4096];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide,
                            (int)(sizeof(wide) / sizeof(wide[0]))) == 0)
        return -1;
    return _wremove(wide);
#else
    return remove(path);
#endif
}

static int test_files(const char *directory)
{
    char path[4096];
    const unsigned char payload[] = {0, 1, 2, 0x0a, 0x0d, 0x1a, 0xff};
    FILE *stream;
    FILE *duplicate;
    int failures = 0;
    int count = snprintf(path, sizeof(path), "%s/capture-platform-"
                         "\xe4\xb8\xad\xe6\x96\x87-\xce\x94.tmp", directory);
    if (count < 0 || (size_t)count >= sizeof(path))
        return expect(0, "test path fits");
    failures += expect(capture_path_exists_utf8(path) == 0, "synthetic path absent");
    stream = capture_open_new_utf8(path);
    if (stream == NULL)
        return failures + expect(0, "create Unicode path");
    failures += expect(fwrite(payload, 1, sizeof(payload), stream) == sizeof(payload),
                       "write binary payload");
    failures += expect(fclose(stream) == 0, "close synthetic file");
    failures += expect(capture_file_size_utf8(path) == (int64_t)sizeof(payload),
                       "Unicode file binary size");
    errno = 0;
    duplicate = capture_open_new_utf8(path);
    failures += expect(duplicate == NULL && errno == EEXIST, "existing path rejected");
    if (duplicate != NULL)
        fclose(duplicate);
    failures += expect(capture_file_size_utf8(path) == (int64_t)sizeof(payload),
                       "failed duplicate did not truncate");
    failures += expect(capture_path_exists_utf8(path) == 1, "existing path found");
    failures += expect(remove_utf8(path) == 0, "remove synthetic file");
    failures += expect(capture_file_size_utf8(path) == -1 && errno == ENOENT,
                       "missing size reports ENOENT");
#ifndef _WIN32
    /* A sparse synthetic file proves the 64-bit stat path without storing GBs. */
    stream = capture_open_new_utf8(path);
    if (stream == NULL) {
        failures += expect(0, "create sparse synthetic file");
    } else {
        const int64_t sparse_size = INT64_C(5) * 1024 * 1024 * 1024 + 7;
        failures += expect(ftruncate(fileno(stream), (off_t)sparse_size) == 0,
                           "set sparse synthetic size");
        failures += expect(fclose(stream) == 0, "close sparse synthetic file");
        failures += expect(capture_file_size_utf8(path) == sparse_size,
                           "size larger than four GiB preserved");
        failures += expect(remove_utf8(path) == 0, "remove sparse synthetic file");
    }
#endif
    failures += expect(capture_path_exists_utf8(NULL) == -1 && errno == EINVAL,
                       "invalid path rejected");
#ifdef _WIN32
    {
        wchar_t *wide[] = {L"test", L"\u4e2d\u6587-\u0394", L"", NULL};
        char **utf8 = capture_argv_utf8(3, wide);
        failures += expect(utf8 != NULL, "wide argv converted");
        if (utf8 != NULL) {
            failures += expect(strcmp(utf8[1], "\xe4\xb8\xad\xe6\x96\x87-\xce\x94") == 0,
                               "argv UTF-8 exact");
            failures += expect(utf8[2][0] == '\0' && utf8[3] == NULL,
                               "empty argument and null terminator preserved");
            capture_free_argv_utf8(3, utf8);
        }
    }
#endif
    printf("files: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures != 0;
}

static int test_stdin(void)
{
    unsigned char buffer[64];
    size_t received = 0;
    uint64_t start = capture_monotonic_ms();
    int saw_timeout = 0;
    for (;;) {
        int count = capture_stdin_poll(buffer + received, sizeof(buffer) - received, 50);
        if (count == -1)
            break;
        if (count == -2)
            return expect(0, "stdin poll succeeds");
        if (count == 0)
            saw_timeout = 1;
        else
            received += (size_t)count;
        if (received == sizeof(buffer) || capture_monotonic_ms() - start > 5000)
            return expect(0, "stdin test completes within five seconds");
    }
    if (expect(received == 5 && memcmp(buffer, "stop\n", 5) == 0,
               "stop command received exactly once"))
        return 1;
    printf("stdin: PASS bytes=%zu timeout=%d elapsed_ms=%" PRIu64 "\n",
           received, saw_timeout, capture_monotonic_ms() - start);
    return 0;
}

static int test_empty_stdin(void)
{
    unsigned char buffer[8];
    uint64_t start = capture_monotonic_ms();
    if (expect(capture_stdin_poll(buffer, sizeof(buffer), 100) == -1,
               "empty closed stdin returns EOF"))
        return 1;
    if (expect(capture_stdin_poll(buffer, sizeof(buffer), 100) == -1,
               "repeated EOF returns immediately"))
        return 1;
    if (expect(capture_monotonic_ms() - start < 1000, "EOF is bounded"))
        return 1;
    puts("empty-stdin: PASS");
    return 0;
}

static int test_terminal(void)
{
    int columns;
    int rows;
    if (expect(capture_terminal_size(&columns, &rows) == 0, "read terminal size"))
        return 1;
    printf("terminal: %d %d\n", columns, rows);
    return 0;
}

#ifndef _WIN32
static int test_posix_pipe(void)
{
    int descriptors[2];
    int saved_stdin;
    int failures = 0;
    unsigned char buffer[16];
    uint64_t start;
    if (pipe(descriptors) != 0)
        return expect(0, "create synthetic pipe");
    saved_stdin = dup(STDIN_FILENO);
    if (saved_stdin < 0 || dup2(descriptors[0], STDIN_FILENO) < 0) {
        if (saved_stdin >= 0)
            close(saved_stdin);
        close(descriptors[0]);
        close(descriptors[1]);
        return expect(0, "redirect test stdin");
    }
    close(descriptors[0]);
    start = capture_monotonic_ms();
    failures += expect(capture_stdin_poll(buffer, sizeof(buffer), 40) == 0,
                       "empty pipe times out");
    failures += expect(capture_monotonic_ms() - start >= 30 &&
                       capture_monotonic_ms() - start < 1000,
                       "pipe timeout is bounded");
    failures += expect(write(descriptors[1], "stop\n", 5) == 5, "write synthetic stop");
    failures += expect(capture_stdin_poll(buffer, sizeof(buffer), 100) == 5 &&
                       memcmp(buffer, "stop\n", 5) == 0, "read synthetic stop");
    close(descriptors[1]);
    failures += expect(capture_stdin_poll(buffer, sizeof(buffer), 100) == -1,
                       "closed pipe reports EOF");
    failures += expect(dup2(saved_stdin, STDIN_FILENO) >= 0, "restore test stdin");
    close(saved_stdin);
    printf("pipe: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures != 0;
}

static int test_posix_wait(void)
{
    int status;
    pid_t child = fork();
    if (child == 0)
        _exit(7);
    if (child < 0)
        return expect(0, "spawn synthetic wait child");
    if (expect(capture_wait_child((intptr_t)child, &status) == 0,
               "wait synthetic child"))
        return 1;
    if (expect(WIFEXITED(status) && WEXITSTATUS(status) == 7, "child exit status"))
        return 1;
    puts("wait: PASS");
    return 0;
}
#endif

int main(int argc, char **argv)
{
    int result;
    capture_init_stdio();
    if (argc == 2 && strcmp(argv[1], "--stdin") == 0)
        return test_stdin();
    if (argc == 2 && strcmp(argv[1], "--empty-stdin") == 0)
        return test_empty_stdin();
    if (argc == 2 && strcmp(argv[1], "--terminal") == 0)
        return test_terminal();
    if (argc != 2) {
        fprintf(stderr, "usage: test_platform temporary-directory | --stdin\n");
        return 2;
    }
    result = test_files(argv[1]);
#ifndef _WIN32
    result |= test_posix_pipe();
    result |= test_posix_wait();
#endif
    return result;
}
