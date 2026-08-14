#include "PsramArena.h"
#include <esp_heap_caps.h>
#include <Arduino.h>
#include <string.h>

// ---- State: all in DRAM, no PSRAM metadata after init() ----
static uint8_t *s_base   = nullptr;
static size_t   s_offset = 0;
static size_t   s_size   = 0;

void PsramArena::init(size_t total_bytes)
{
    // Prefer 8-bit-accessible allocation; fall back to any SPIRAM.
    s_base = (uint8_t *)heap_caps_malloc(total_bytes,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_base)
        s_base = (uint8_t *)heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM);

    s_size   = s_base ? total_bytes : 0;
    s_offset = 0;

    Serial.printf("[Arena] PSRAM arena init: base=%p  size=%u  SPIRAM_free_after=%u\n",
                  s_base, (unsigned)s_size,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    Serial.flush();

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
    return p;
}

