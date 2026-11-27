// ============================================================================
// UpdFile.cpp - see UpdFile.h.
// ============================================================================
#ifndef SIMULATOR
#include "UpdFile.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace UpdFile {
namespace {

// The mount point Config.cpp passes to LittleFS.begin().
constexpr const char *MOUNT = "/littlefs";

// "/littlefs" + path into `out`; false if it does not fit (no update path
// name comes close to that).
bool vfsPath(char *out, size_t cap, const char *path) {
    const int n = snprintf(out, cap, "%s%s", MOUNT, path);
    return n > 0 && (size_t)n < cap;
}

}  // namespace

int openRead(const char *path) {
    char p[64];
    return vfsPath(p, sizeof(p), path) ? ::open(p, O_RDONLY) : -1;
}

int openWrite(const char *path) {
    char p[64];
    return vfsPath(p, sizeof(p), path) ? ::open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
}

bool readExact(int fd, uint8_t *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        const ssize_t k = ::read(fd, buf + got, n - got);
        if (k <= 0) return false;
        got += (size_t)k;
    }
    return true;
}

bool writeAll(int fd, const uint8_t *buf, size_t n) {
    size_t put = 0;
    while (put < n) {
        const ssize_t k = ::write(fd, buf + put, n - put);
        if (k <= 0) return false;
        put += (size_t)k;
    }
    return true;
}

bool seekTo(int fd, uint32_t pos) {
    return ::lseek(fd, (off_t)pos, SEEK_SET) == (off_t)pos;
}

void closeFd(int fd) {
    if (fd >= 0) ::close(fd);
}

int32_t fileSize(const char *path) {
    char p[64];
    struct stat st;
    if (!vfsPath(p, sizeof(p), path) || ::stat(p, &st) != 0) return -1;
    return (int32_t)st.st_size;
}

bool exists(const char *path) { return fileSize(path) >= 0; }

bool removeFile(const char *path) {
    char p[64];
    return vfsPath(p, sizeof(p), path) && ::unlink(p) == 0;
}

int renameFile(const char *from, const char *to) {
    char a[64], b[64];
    if (!vfsPath(a, sizeof(a), from) || !vfsPath(b, sizeof(b), to)) return EINVAL;
    errno = 0;
    if (::rename(a, b) == 0) return 0;
    return errno ? errno : EIO;
}

}  // namespace UpdFile
#endif  // SIMULATOR
