#pragma once
/**
 * PsramArena – single-allocation PSRAM bump allocator.
 *
 * Problem: ESP-IDF v4.4.7 OPI PSRAM TLSF metadata lives IN PSRAM.
 * Heavy DRAM realloc() pressure (LVGL style arrays) causes OPI cache
 * lines for that metadata to be evicted.  The next heap_caps_malloc(SPIRAM)
 * then reads stale/uninitialized physical PSRAM → block_locate_free assert.
 *
 * Solution: call heap_caps_malloc(SPIRAM) exactly ONCE for the entire
 * canvas-buffer arena.  All subsequent sub-allocations are pure pointer
 * arithmetic – the offset is kept in DRAM (no PSRAM TLSF access ever again).
 *
 * Limitations:
 *   - Never frees.  Fine for canvas buffers that live for the app lifetime.
 *   - 4-byte aligned.
 *   - Thread-safe only if called from a single task (setup() context).
 */

#include <stddef.h>
#include <stdint.h>

namespace PsramArena {

/** One-time init.  Call from setup(), before any screen create().
 *  Makes exactly one heap_caps_malloc(SPIRAM) of @p total_bytes. */
void init(size_t total_bytes);

/** One-time init for the 1024x600 boards, instead of init(): PSRAM is full
 *  there, so the panel's frame buffer gets its room FIRST. Asks for
 *  @p total_bytes, but takes less - in 16 KB steps - when that would leave
 *  no room for one block of @p room_bytes (@p room_caps) next to it, or less
 *  than @p keep_free bytes of PSRAM free besides. That block is held until
 *  releaseRoom() (MALLOC_CAP_DMA is ignored in @p room_caps: with it, IDF
 *  would hold a block too small to be found again). A short arena only
 *  means the screens built last find no
 *  canvas buffer, which they survive; a frame buffer that does not fit means
 *  a dark panel and a setup() that never finishes. */
void initLeavingRoom(size_t total_bytes, size_t room_bytes, uint32_t room_caps,
                     size_t keep_free);

/** Frees the block initLeavingRoom() held. Call right before the panel
 *  allocates its frame buffer, so nothing else can take the room first. */
void releaseRoom();

/** Bump-allocate @p bytes (4-byte aligned) from the arena.
 *  Returns nullptr if the arena is exhausted. */
void *alloc(size_t bytes);

/** What the arena got, what it asked for, and how much of it is handed out. */
size_t size();
size_t wanted();
size_t used();

} // namespace PsramArena
