#include "PsramArena.h"
#include <esp_heap_caps.h>
#include <Arduino.h>
#include <string.h>

// ---- State: all in DRAM, no PSRAM metadata after init() ----
static uint8_t *s_base   = nullptr;
static size_t   s_offset = 0;
static size_t   s_size   = 0;
static size_t   s_wanted = 0;
static void    *s_room   = nullptr;   // initLeavingRoom(): the frame buffer's place

static void zeroFill();

void PsramArena::init(size_t total_bytes)
{
    // Prefer 8-bit-accessible allocation; fall back to any SPIRAM.
    s_base = (uint8_t *)heap_caps_malloc(total_bytes,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_base)
        s_base = (uint8_t *)heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM);

    s_size   = s_base ? total_bytes : 0;
    s_wanted = total_bytes;
    s_offset = 0;

    Serial.printf("[Arena] PSRAM arena init: base=%p  size=%u  SPIRAM_free_after=%u\n",
                  s_base, (unsigned)s_size,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    Serial.flush();
    zeroFill();
}

#ifndef SIMULATOR
// On the 1024x600 boards the firmware's code and read-only data run from PSRAM
// as well, in whole 64 KB pages, and what is left after the arena and the
// panel's 1.2 MB frame buffer is a few dozen KB. The arena used to come first
// and the frame buffer after it, so one more page of code left the panel dark
// (measured 02.11.2026: a build ~10 KB larger). Now the arena makes room.
//
// Two things about ESP-IDF 5's heap (TLSF) decide how this has to be done:
//
//  * A plain malloc is handed its whole size class - classes are 1/32 of a
//    power of two, so 128 KB wide at the arena's size. heap_caps_malloc() of
//    4,683,168 bytes took 4,718,592, and asking for a little less freed
//    nothing. An ALIGNED allocation is cut to the bytes asked for (tlsf.c,
//    tlsf_memalign_offs() hands block_prepare_used() the unrounded size), so
//    the arena is taken that way: ~35 KB back, and every byte it gives up
//    really is free afterwards.
//
//  * Any allocation only looks at free blocks of at least the class it rounds
//    up to. The panel's frame buffer (also an aligned allocation) needs a free
//    block of 1,245,184 bytes and then occupies only 1,228,8xx. So the room is
//    held as a PLAIN malloc - that gets the whole class, and freed again it is
//    a block the panel's request is guaranteed to find, wherever it sits.
//    Held as an aligned block it comes back 16 KB too small to be found once
//    anything has settled right behind it - which is what happened on the 7B
//    when the hold still asked for MALLOC_CAP_DMA: IDF routes every DMA
//    request through the aligned path (esp_heap_adjust_alignment_to_hw), so
//    the caller has to leave that flag out (DisplaySetup.h).
//
// Taking the arena, then the room, then giving both back and trying again
// 16 KB smaller is cheap at this point of the boot: nothing else is using
// PSRAM yet, and TLSF merges the two freed blocks back into one.
void PsramArena::initLeavingRoom(size_t total_bytes, size_t room_bytes,
                                 uint32_t room_caps, size_t keep_free)
{
    static constexpr size_t ARENA_ALIGN = 16;          // anything above 4 takes the trimmed path
    static constexpr size_t STEP        = 16 * 1024;
    room_caps &= ~(uint32_t)MALLOC_CAP_DMA;           // the hold must take the plain path, see above

    const size_t freeBefore = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    Serial.printf("[Arena] PSRAM heap: %u bytes, %u free before the arena\n",
                  (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                  (unsigned)freeBefore);

    s_wanted = total_bytes;
    s_offset = 0;
    size_t size = total_bytes;
    for (;;) {
        s_base = (uint8_t *)heap_caps_aligned_alloc(ARENA_ALIGN, size,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_room = s_base ? heap_caps_malloc(room_bytes, room_caps) : nullptr;
        if (s_room && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= keep_free) break;
        if (s_room) heap_caps_free(s_room);
        if (s_base) heap_caps_free(s_base);
        s_room = nullptr;
        s_base = nullptr;
        if (size <= STEP) {
            // Not even the smallest arena fits next to the frame buffer. The
            // picture still comes first: no arena at all (the DataModel falls
            // back to malloc, canvases stay empty), but a panel that lights up
            // and a device that can be reached and updated.
            s_room = heap_caps_malloc(room_bytes, room_caps);
            size = 0;
            break;
        }
        size -= STEP;
    }
    s_size = s_base ? size : 0;

    Serial.printf("[Arena] PSRAM arena: %u of %u bytes at %p, frame buffer room %s,"
                  " %u bytes PSRAM free besides\n",
                  (unsigned)s_size, (unsigned)total_bytes, s_base,
                  s_room ? "held" : "NOT held",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (s_size < total_bytes)
        Serial.printf("[Arena] !!! PSRAM SHORT: arena %u bytes smaller than asked - "
                      "the screens built last get no canvas\n",
                      (unsigned)(total_bytes - s_size));
    Serial.flush();
    zeroFill();
}

void PsramArena::releaseRoom()
{
    if (s_room) heap_caps_free(s_room);
    s_room = nullptr;
}
#endif  // !SIMULATOR

static void zeroFill()
{
    // Zero-fill the entire arena HERE, before WiFi starts.
    //
    // Canvas screens sub-allocate from this arena in DisplayManager::activate(),
    // which runs AFTER webCfg.begin() (WiFi already up).  If we zero each canvas
    // buffer there, WiFi DMA is competing for the OPI PSRAM bus.  The bus
    // arbitration drops effective write speed to ~460 KB/s; a 412 KB canvas
    // takes ~900 ms to zero.  During those 900 ms the WiFi ISR (level-3) stalls
    // waiting for a SPI0 flash fetch, which blocks the FreeRTOS tick (level-1).
    // The Interrupt WDT (TG1) detects the missing tick and fires TG1WDT_SYS_RST.
    //
    // Before WiFi, the OPI bus is uncontested.  Without level-3 ISR stalls the
    // FreeRTOS tick runs normally and the IWDT is never starved.
    if (s_base) {
        Serial.print("[Arena] zero-fill... "); Serial.flush();
        uint32_t t0 = millis();
        memset(s_base, 0, s_size);
        Serial.printf("done in %u ms\n", millis() - t0); Serial.flush();
    }
}

void *PsramArena::alloc(size_t bytes)
{
    if (!s_base) return nullptr;

    // 4-byte align the request
    size_t aligned = (bytes + 3u) & ~3u;

    if (s_offset + aligned > s_size) {
        Serial.printf("[Arena] OOM: want %u, only %u remain\n",
                      (unsigned)bytes, (unsigned)(s_size - s_offset));
        Serial.flush();
        return nullptr;
    }

    void *p = s_base + s_offset;
    s_offset += aligned;
#ifdef SIMULATOR
    // PC only: per-allocation trace for sizing the device arena.
    Serial.printf("[Arena] alloc %u  (used %u / %u)\n",
                  (unsigned)bytes, (unsigned)s_offset, (unsigned)s_size);
#endif
    return p;
}

size_t PsramArena::size()   { return s_size; }
size_t PsramArena::wanted() { return s_wanted; }
size_t PsramArena::used()   { return s_offset; }

