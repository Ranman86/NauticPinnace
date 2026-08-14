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

/** Bump-allocate @p bytes (4-byte aligned) from the arena.
 *  Returns nullptr if the arena is exhausted. */
void *alloc(size_t bytes);

/** Remaining free bytes. */

} // namespace PsramArena
