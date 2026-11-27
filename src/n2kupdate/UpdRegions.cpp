// ============================================================================
// UpdRegions.cpp - see UpdRegions.h.
// ============================================================================
#ifndef SIMULATOR
#include "UpdRegions.h"
#include "UpdCrypto.h"
#include "UpdFile.h"
#include "UpdNvs.h"
#include "../PsramArena.h"
#include <Arduino.h>
#include <string.h>

namespace UpdRegions {
namespace {

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

uint8_t *s_region[2]  = { nullptr, nullptr };
uint8_t *s_recvBlock  = nullptr;      // 1 KB for the receiver object
bool     s_reserved   = false;

// The bounce buffer: .bss is internal RAM on every board, also with XIP from
// PSRAM on the 7B/5B. 4-byte alignment keeps esp_flash on its direct path.
alignas(4) uint8_t s_bounce[n2kupd::BOUNCE_BYTES];

// One descriptor per region and a single pointer to the live one (9.4.5).
// Publishing is that one aligned pointer store, so a handler on another core
// always reads pointer, length and gzip flag of the same cache.
Cache                 s_desc[2];
const Cache *volatile s_live    = nullptr;
int8_t                s_liveIdx = -1;

// Grace of a retired cache region (9.1): a response started before the swap
// may still be reading it.
bool     s_graceOn[2]    = { false, false };
uint32_t s_graceStart[2] = { 0, 0 };

bool   s_workHeld = false;
int8_t s_workIdx  = -1;

// The stored web UI (INFO bytes 97-128).
uint8_t  s_webId[8]   = { 0 };
uint32_t s_webSize    = 0;
uint8_t  s_webVer[16] = { 0 };

inline int8_t workIndexLocked() {
    // The region that does not hold the live cache. Without a live cache the
    // work region is region 1, so region 0 stays where a cache gets loaded.
    return s_liveIdx == 1 ? 0 : 1;
}

}  // namespace

bool reserve() {
    if (s_reserved) return true;
    // The receiver block first: should the regions not fit, the receiver
    // still answers INFO with capabilities 0 and OFFERs with OUT_OF_MEMORY.
    if (!s_recvBlock) s_recvBlock = (uint8_t *)PsramArena::alloc(ARENA_BYTES - 2 * REGION_BYTES);
    s_region[0] = (uint8_t *)PsramArena::alloc(REGION_BYTES);
    s_region[1] = (uint8_t *)PsramArena::alloc(REGION_BYTES);
    s_reserved = s_region[0] && s_region[1];
    Serial.printf("[n2kupd] update regions %s: 2 x %u B at %p / %p\n",
                  s_reserved ? "reserved" : "NOT reserved (arena too small)",
                  (unsigned)REGION_BYTES, s_region[0], s_region[1]);
    return s_reserved;
}

bool reserved() { return s_reserved; }

void *receiverStorage(size_t bytes, size_t align) {
    if (!s_recvBlock || !align || (align & (align - 1))) return nullptr;
    // PsramArena::alloc only promises 4-byte alignment; the receiver has
    // 64-bit members (alignof 8), so the object starts at the next multiple.
    const uintptr_t base = (uintptr_t)s_recvBlock;
    const uintptr_t at = (base + align - 1) & ~(uintptr_t)(align - 1);
    const size_t room = ARENA_BYTES - 2 * REGION_BYTES;
    return (at - base) + bytes <= room ? (void *)at : nullptr;
}

uint8_t *bounce() { return s_bounce; }

uint32_t acquireWork(n2kupd::WorkArea &out) {
    memset(&out, 0, sizeof(out));
    if (!s_reserved) return 0;          // window stays null: the receiver fails cleanly
    portENTER_CRITICAL(&s_mux);
    // One holder at a time. UpdateGate already keeps a WiFi upload and an N2K
    // session apart; this makes a second grant impossible all the same.
    if (s_workHeld) {
        portEXIT_CRITICAL(&s_mux);
        return 1000;
    }
    const int8_t idx = workIndexLocked();
    if (s_graceOn[idx]) {
        const uint32_t e = millis() - s_graceStart[idx];
        if (e < n2kupd::T_REGION_GRACE_MS) {
            portEXIT_CRITICAL(&s_mux);
            const uint32_t left = n2kupd::T_REGION_GRACE_MS - e;
            return left ? left : 1;
        }
        s_graceOn[idx] = false;
    }
    s_workHeld = true;
    s_workIdx  = idx;
    portEXIT_CRITICAL(&s_mux);
    uint8_t *b = s_region[idx];
    out.window        = b + WINDOW_OFFSET;
    out.inflaterState = b + INFLATER_STATE_OFFSET;
    out.dict          = b + DICT_OFFSET;
    return 0;
}

void releaseWork() {
    portENTER_CRITICAL(&s_mux);
    s_workHeld = false;
    s_workIdx  = -1;
    portEXIT_CRITICAL(&s_mux);
}

uint8_t *workBase() {
    portENTER_CRITICAL(&s_mux);
    uint8_t *b = s_workHeld ? s_region[s_workIdx] : nullptr;
    portEXIT_CRITICAL(&s_mux);
    return b;
}

const Cache *liveCache() { return s_live; }

bool publishWork(uint32_t len) {
    if (!len || len > REGION_BYTES) return false;
    portENTER_CRITICAL(&s_mux);
    if (!s_workHeld) { portEXIT_CRITICAL(&s_mux); return false; }
    const int8_t idx = s_workIdx;
    const int8_t old = s_liveIdx;
    // This descriptor is not live (its region was the work region), so
    // nothing reads it while it is filled.
    s_desc[idx].buf  = s_region[idx];
    s_desc[idx].len  = len;
    s_desc[idx].gzip = true;
    s_live    = &s_desc[idx];           // THE publication: one pointer store
    s_liveIdx = idx;
    if (old >= 0) { s_graceOn[old] = true; s_graceStart[old] = millis(); }
    portEXIT_CRITICAL(&s_mux);
    return true;
}

// Reads a whole file through the bounce buffer, hashing it; copies it into
// `dst` as well when that is not null. Returns false on a read error.
// Unbuffered (UpdFile): Arduino's File would put a 4 KB stdio buffer between
// LittleFS and the bounce buffer (9.1).
static bool hashFile(int fd, uint32_t len, uint8_t *dst, uint8_t sha[32]) {
    MbedSha256 h;
    h.init();
    uint32_t pos = 0;
    while (pos < len) {
        const size_t want = (len - pos) < n2kupd::BOUNCE_BYTES ? (len - pos) : n2kupd::BOUNCE_BYTES;
        if (!UpdFile::readExact(fd, s_bounce, want)) return false;
        h.update(s_bounce, want);
        if (dst) memcpy(dst + pos, s_bounce, want);
        pos += (uint32_t)want;
    }
    h.finish(sha);
    return true;
}

void bootLoad() {
    // 9.4.8: an update that died between write and rename leaves this behind.
    if (UpdFile::exists("/index.html.gz.new")) {
        UpdFile::removeFile("/index.html.gz.new");
        Serial.println("[web] stale /index.html.gz.new removed");
    }
    memset(s_webId, 0, sizeof(s_webId));
    memset(s_webVer, 0, sizeof(s_webVer));
    s_webSize = 0;

    const int32_t size = UpdFile::fileSize("/index.html.gz");
    const int fd = size >= 0 ? UpdFile::openRead("/index.html.gz") : -1;
    if (fd < 0) {
        Serial.println("[web] no /index.html.gz - the plain /index.html is served from flash");
        return;
    }
    const uint32_t len = (uint32_t)size;
    uint8_t sha[32];
    // Into region 0, the region kept for the cache while nothing is live.
    const bool fits = s_reserved && len > 0 && len <= REGION_BYTES;
    const bool ok = len > 0 && hashFile(fd, len, fits ? s_region[0] : nullptr, sha);
    UpdFile::closeFd(fd);
    if (!ok) {
        Serial.println("[web] /index.html.gz could not be read");
        return;
    }
    memcpy(s_webId, sha, 8);
    s_webSize = len;
    if (fits) {
        portENTER_CRITICAL(&s_mux);
        s_desc[0].buf  = s_region[0];
        s_desc[0].len  = len;
        s_desc[0].gzip = true;
        s_live    = &s_desc[0];
        s_liveIdx = 0;
        portEXIT_CRITICAL(&s_mux);
    }
    // 9.4.7: the version is only known for a file this firmware installed
    // itself; after a littlefs.bin flashed over USB the stored id no longer
    // matches and the version stays empty.
    UpdNvs::WebVer wv;
    if (UpdNvs::loadWebVer(wv) && memcmp(wv.id, s_webId, 8) == 0)
        memcpy(s_webVer, wv.version, 16);
    Serial.printf("[web] index.html.gz %s (%u bytes), web UI id %02X%02X%02X%02X%02X%02X%02X%02X\n",
                  fits ? "cached in PSRAM" : "too large for the cache - served from flash",
                  (unsigned)len, s_webId[0], s_webId[1], s_webId[2], s_webId[3],
                  s_webId[4], s_webId[5], s_webId[6], s_webId[7]);
}

void webUiIdentity(uint8_t id[8], uint32_t &size, uint8_t version[16]) {
    portENTER_CRITICAL(&s_mux);
    memcpy(id, s_webId, 8);
    size = s_webSize;
    memcpy(version, s_webVer, 16);
    portEXIT_CRITICAL(&s_mux);
}

void setWebUiIdentity(const uint8_t id[8], uint32_t size, const uint8_t version16[16]) {
    portENTER_CRITICAL(&s_mux);
    memcpy(s_webId, id, 8);
    s_webSize = size;
    if (version16) memcpy(s_webVer, version16, 16);
    else           memset(s_webVer, 0, 16);
    portEXIT_CRITICAL(&s_mux);
    UpdNvs::WebVer wv;
    memcpy(wv.id, id, 8);
    if (version16) memcpy(wv.version, version16, 16);
    else           memset(wv.version, 0, 16);
    UpdNvs::storeWebVer(wv);
}

}  // namespace UpdRegions
#endif  // SIMULATOR
