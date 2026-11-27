#pragma once
// ============================================================================
// UpdFile - unbuffered file access for the update path (docs/N2K-UPDATE.md
// 9.1, 9.4).
//
// WHY NOT Arduino's File: it goes through newlib stdio, and Arduino gives
// every opened file a 4 KB stdio buffer from malloc (vfs_api.cpp setvbuf, or
// newlib's own from st_blksize = 4096). That buffer is internal RAM held for a
// whole web UI session - or PSRAM, when no internal 4 KB block is free - and
// every 256-byte read refills all 4 KB of it, with LittleFS reading flash
// straight into it (lfs_bd_read bypasses its cache for aligned reads). Both
// break the bounce rule of 9.1: no PSRAM pointer to LittleFS, every read and
// write through the one 256-byte internal bounce buffer.
//
// These calls go to the VFS directly - open / read / write / lseek / close on
// "/littlefs/..." - so the bytes move between LittleFS and the caller's
// buffer, which in the update path is always the bounce buffer. Paths are
// LittleFS paths ("/index.html.gz"); the mount point is added here.
// ============================================================================
#include <stdint.h>
#include <stddef.h>

namespace UpdFile {

int     openRead(const char *path);                       // fd >= 0, or -1
int     openWrite(const char *path);                      // create or truncate; fd, or -1
bool    readExact(int fd, uint8_t *buf, size_t n);        // exactly n bytes
bool    writeAll(int fd, const uint8_t *buf, size_t n);   // exactly n bytes
bool    seekTo(int fd, uint32_t pos);
void    closeFd(int fd);
int32_t fileSize(const char *path);                       // -1 if missing
bool    exists(const char *path);
bool    removeFile(const char *path);
// 0, or the errno: EBUSY while either name is open (a slow client on the
// fallback route), which is the one failure worth waiting for (9.4.4).
int     renameFile(const char *from, const char *to);

}  // namespace UpdFile
