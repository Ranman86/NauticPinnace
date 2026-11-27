#pragma once
// ============================================================================
// UpdRegions - the memory of the update path (docs/N2K-UPDATE.md 9.1, 9.4).
//
// PSRAM: TWO REGIONS OF UPD_REGION_BYTES, RESERVED ONCE AT BOOT. IDF 4.4.7
// with OPI PSRAM can assert in block_locate_free on a runtime
// heap_caps_malloc(SPIRAM) (PsramArena.h), so nothing of the update path
// allocates PSRAM at runtime, on any board. reserve() takes both regions out
// of the PsramArena in setup(), directly after data.initBuffers() and before
// any other task can touch the arena; main.cpp grows the arena by exactly
// that much, so no existing slack is consumed.
//
// At any time one region holds the live web UI cache (what "/" serves), and
// the other is the WORK REGION of an update session:
//
//   [0, 13824)       window buffer, 64 chunks of 216 bytes
//   [13824, 32768)   tinfl_decompressor (about 11 KB, RomInflater)
//   [32768, 65536)   the 32 KB deflate dictionary
//
// A web UI update reloads the new file into the work region and then
// PUBLISHES it: the two regions swap roles with one aligned pointer store.
// The HTTP handler hands out a raw pointer into the cache that a running
// response keeps reading (AsyncProgmemResponse), so the retired region becomes
// the work region only after a 120 s grace.
//
// INTERNAL RAM: the one 256-byte bounce buffer (bounce rule, 9.1). Every
// flash or file read and write and every SHA update of the update path goes
// through it. It belongs to whoever holds the writer (UpdateGate) - and at
// boot, before any task runs, to OtaGuard and the web UI load.
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include "N2kUpdateCore.h"

// 128 KB per region on every board (owner decision 15.2 #8). Hardware test
// H25 decides whether the 1024x600 boards drop to 96 KB; INFO byte 25 always
// reports the real value, so only this line would change.
#ifndef UPD_REGION_BYTES
#define UPD_REGION_BYTES 131072u
#endif

namespace UpdRegions {

static constexpr uint32_t REGION_BYTES         = UPD_REGION_BYTES;
static constexpr uint32_t WINDOW_OFFSET        = 0;
static constexpr uint32_t INFLATER_STATE_OFFSET = n2kupd::WINDOW_BYTES;           // 13,824
static constexpr uint32_t DICT_OFFSET          = 32768;
static constexpr uint32_t INFLATER_STATE_BYTES = DICT_OFFSET - INFLATER_STATE_OFFSET;
static_assert(DICT_OFFSET + n2kupd::DICT_BYTES <= REGION_BYTES, "work layout exceeds the region");
// Arena bytes reserve() takes: two regions plus room for the receiver object,
// which lives in PSRAM too (the session record, 9.1).
static constexpr uint32_t ARENA_BYTES          = 2 * REGION_BYTES + 1024;

// setup(), right after data.initBuffers(). False if the arena is too small;
// capabilities are then 0 and every OFFER gets OUT_OF_MEMORY (9.1).
bool reserve();
bool reserved();

// The 1 KB block reserve() took for the receiver object: `bytes` at the first
// address in it that is a multiple of `align` (a power of two), or null.
void *receiverStorage(size_t bytes, size_t align);

// 256 bytes, internal RAM, 4-byte aligned.
uint8_t *bounce();

// ---- work region (9.1) ---------------------------------------------------------
// 0: granted, `out` filled. Otherwise the milliseconds of grace left before the
// region retired by the last web UI swap may be reused.
uint32_t acquireWork(n2kupd::WorkArea &out);
void     releaseWork();
uint8_t *workBase();          // base of the held work region, null if none held

// ---- web UI cache (9.4.5) --------------------------------------------------------
struct Cache {
    const uint8_t *buf;
    uint32_t       len;
    bool           gzip;
};
// The live cache: one aligned pointer load, never torn. Null = none loaded;
// the HTTP handler then falls back to LittleFS.
const Cache *liveCache();
// The held work region holds a complete gzip web UI of `len` bytes: make it
// the live cache. The previous cache region enters its 120 s grace.
bool publishWork(uint32_t len);

// ---- web UI identity (INFO bytes 97-128, /api/info) --------------------------------
// setup(), after LittleFS is mounted: deletes a stale /index.html.gz.new
// (9.4.8), loads /index.html.gz into a region and computes its web UI id.
void bootLoad();
void webUiIdentity(uint8_t id[8], uint32_t &size, uint8_t version[16]);
// After a web UI was replaced: the new id and size, and NVS n2kupd/webver
// = {id, version} so the version survives a restart (9.4.7).
void setWebUiIdentity(const uint8_t id[8], uint32_t size, const uint8_t version16[16]);

}  // namespace UpdRegions
