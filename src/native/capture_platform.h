/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CAPTURE_PLATFORM_H
#define CAPTURE_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef _WIN32
#include <wchar.h>

/* Windows capture supports raw output; the child-wait API is unsupported. */
#ifndef WIFEXITED
#define WIFEXITED(status) (1)
#endif
#ifndef WEXITSTATUS
#define WEXITSTATUS(status) (status)
#endif
#else
#include <sys/wait.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Creates a binary file exclusively. Existing paths are never truncated. */
FILE *capture_open_new_utf8(const char *path);

/* Returns the size, or -1 with errno set. Paths are UTF-8 on Windows. */
int64_t capture_file_size_utf8(const char *path);

/* Returns 1 for an existing path, 0 for an absent path, or -1 on error. */
int capture_path_exists_utf8(const char *path);

/*
 * Returns bytes read (>0), timeout/no input (0), EOF (-1), or error (-2).
 * timeout_ms <= 0 means an immediate check. The caller must be the only stdin
 * reader. Windows GUI pipes are supported. Console input waits at most the
 * requested timeout and returns 0 without consuming records; stopping through
 * stdin therefore requires a redirected pipe on Windows.
 */
int capture_stdin_poll(unsigned char *buffer, size_t capacity, int timeout_ms);

void capture_sleep_ms(int milliseconds);
void capture_init_stdio(void);
/* Monotonic milliseconds; UINT64_MAX with errno set on a POSIX clock error. */
uint64_t capture_monotonic_ms(void);

/* Returns 0 on success, -1 with errno set on failure. */
int capture_wait_child(intptr_t child, int *status);
int capture_terminal_size(int *columns, int *rows);

#ifdef _WIN32
/* argc + 1 entries, including a trailing NULL; conversion is strict UTF-8. */
char **capture_argv_utf8(int argc, wchar_t *const argv[]);
void capture_free_argv_utf8(int argc, char **argv);
#endif

#ifdef __cplusplus
}
#endif

#endif
