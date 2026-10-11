/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BES_HOST_FILE_H
#define BES_HOST_FILE_H
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <glib/gstdio.h>
#ifndef O_BINARY
#define O_BINARY 0
#endif
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC O_NOINHERIT
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_NONBLOCK
/* These descriptors are regular persistence files, never pipes/devices. */
#define O_NONBLOCK 0
#endif
static inline int bes_file_lock(int fd)
{
    OVERLAPPED ov = {0};
    if (LockFileEx((HANDLE)_get_osfhandle(fd), LOCKFILE_EXCLUSIVE_LOCK |
                   LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &ov)) return 0;
    errno = EWOULDBLOCK; return -1;
}
static inline int bes_data_sync(int fd) { return _commit(fd); }
static inline int bes_dir_sync(const char *path) { (void)path; return 0; }
static inline int bes_replace(const char *from, const char *to)
{
    gunichar2 *wide_from = g_utf8_to_utf16(from, -1, NULL, NULL, NULL);
    gunichar2 *wide_to = g_utf8_to_utf16(to, -1, NULL, NULL, NULL);
    BOOL ok = FALSE;
    DWORD error = ERROR_INVALID_NAME;
    if (wide_from && wide_to) {
        /* A reader without FILE_SHARE_DELETE can temporarily block atomic
         * replacement on Windows (also reported as ACCESS_DENIED). Keep the
         * old snapshot intact and bound the retry; never truncate in place. */
        for (unsigned attempt = 0; attempt < 26; attempt++) {
            ok = MoveFileExW((WCHAR *)wide_from, (WCHAR *)wide_to,
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            error = ok ? ERROR_SUCCESS : GetLastError();
            if (ok || (error != ERROR_SHARING_VIOLATION &&
                       error != ERROR_ACCESS_DENIED) || attempt == 25) {
                break;
            }
            Sleep(10);
        }
    }
    g_free(wide_from); g_free(wide_to);
    if (ok) return 0;
    errno = error == ERROR_SHARING_VIOLATION ? EBUSY :
            error == ERROR_ACCESS_DENIED ? EACCES : EIO;
    SetLastError(error);
    return -1;
}
#else
#include <sys/file.h>
static inline int bes_file_lock(int fd) { return flock(fd, LOCK_EX | LOCK_NB); }
static inline int bes_data_sync(int fd)
{
#ifdef __APPLE__
    return fsync(fd);
#else
    return fdatasync(fd);
#endif
}
static inline int bes_dir_sync(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    int result = fsync(fd);
    close(fd); return result;
}
static inline int bes_replace(const char *from, const char *to) { return rename(from, to); }
#endif
#endif
