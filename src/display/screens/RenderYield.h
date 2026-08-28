#pragma once
#include <Arduino.h>   // millis / vTaskDelay / pdMS_TO_TICKS (sim: sim/arduino_stubs.h)

// ============================================================
// RenderYield.h - the ONE yield point shared by every canvas screen.
//
// Why the screens yield at all
// ----------------------------
// LVGL's canvas draw calls have no yield point inside them, and a repaint
// writes hundreds of kilobytes straight into PSRAM. So every fill loop in
// these screens used to end with vTaskDelay(pdMS_TO_TICKS(1)). The comments
// that came with those calls name two things being defended against:
//
//   1. the Interrupt WDT (TG1 MWDT) firing after the WiFi beacon ISR had
//      spun ~800 ms waiting for SPI0, which the CPU was holding for its own
//      PSRAM cache-line fills  ->  TG1WDT_SYS_RST; and
//   2. the beacon ISR simply missing its SPI0 window.
//
// Why that reasoning does not hold on the 1024x600 boards
// -------------------------------------------------------
// Premise 1 is void: src/main.cpp turns the TG1 MWDT OFF as the very first
// statement of setup() - the unlock-key block writing
// TIMERG1.wdtconfig0.wdt_en = 0, before Serial.begin() and long before any
// screen exists. The watchdog those comments are afraid of cannot fire.
//
// Premise 2 came from the 4-inch board, whose display hangs off SPI. The 7B
// and the 5B drive a parallel RGB panel (src/DisplaySetup_7B.cpp builds it
// with esp_lcd_new_rgb_panel), so the display never contends for SPI0 here
// in the first place.
//
// What the old shape cost
// -----------------------
// CONFIG_FREERTOS_HZ is 1000, so pdMS_TO_TICKS(1) is exactly one tick and
// vTaskDelay(1) blocks until the NEXT tick edge. In a loop whose body is much
// shorter than a tick that phase-locks: wake on the edge, work ~0.2 ms, sleep
// the remaining ~0.8 ms. Every iteration then costs a whole millisecond no
// matter how little it does. WindScreen's background fill alone runs 75 of
// them (600 rows in 8-row chunks) and the layer-by-layer yields add more, so
// a repaint spent roughly 130 ms asleep on top of 25-45 ms of real drawing.
//
// What this does instead
// ----------------------
// On the panel boards renderYield() yields at most once per
// RENDER_YIELD_PERIOD_MS of wall clock, however often it is called. About 130
// sleeps per repaint become about 8 and the sleeping stops dominating the
// frame. It is deliberately NOT "stop yielding": the paint still hands the
// core back several times a frame, so the idle task and anything else pinned
// to Core 1 keep running - the point is only to stop paying a full tick for a
// yield that a microsecond of work has earned.
//
// The 4-inch board is a released product. It takes the #else branch, which is
// the original unconditional vTaskDelay with the original argument, so its
// behaviour and its object code are unchanged.
//
// One timestamp is shared by ALL callers on purpose (it lives in
// CanvasDraw.cpp). A repaint crosses several translation units - a screen's
// own fill loops plus cdFillPoly() in CanvasDraw.cpp - and a per-file
// timestamp would let each of them yield once per window independently.
// ============================================================

#if defined(BOARD_PANEL_1024X600)

// Shortest gap between two real yields, in milliseconds of wall clock.
// 20 ms is ~one yield per 8 kB-ish worth of fill work at PSRAM speed: short
// enough that nothing waiting on this core notices, long enough that the
// tick-edge sleep is no longer the bulk of the frame.
static constexpr uint32_t RENDER_YIELD_PERIOD_MS = 20;

// millis() of the last yield actually taken. Defined in CanvasDraw.cpp.
extern uint32_t g_renderYieldLastMs;

#endif  // BOARD_PANEL_1024X600

// Yield out of a canvas draw loop. Call it as often as the old code called
// vTaskDelay - it decides for itself whether this one is worth a tick.
// ms is the sleep length the call site used to ask for (all but GridScreen
// want 1); it is honoured whenever the yield is actually taken.
static inline void renderYield(uint32_t ms = 1) {
#if defined(BOARD_PANEL_1024X600)
    const uint32_t now = millis();
    // Unsigned subtraction, so this stays correct across the 49-day millis()
    // wrap instead of going quiet for a month.
    if ((uint32_t)(now - g_renderYieldLastMs) < RENDER_YIELD_PERIOD_MS) return;
    g_renderYieldLastMs = now;
    vTaskDelay(pdMS_TO_TICKS(ms));
#else
    vTaskDelay(pdMS_TO_TICKS(ms));   // 4-inch board: unchanged, see above
#endif
}
