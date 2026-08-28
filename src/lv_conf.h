/**
 * @file lv_conf.h
 * LVGL 8.x configuration for the marine display project.
 * Based on lv_conf_template.h from LVGL 8.x, Copyright (c) 2021 LVGL Kft,
 * MIT — see THIRD-PARTY-NOTICES.md and LICENSES/MIT-LVGL.txt.
 * Place next to lv_lvgl.h (or in include path with LV_CONF_INCLUDE_SIMPLE).
 */
#if 1  /* Set to "1" to enable the content */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0
#define LV_COLOR_SCREEN_TRANSP 0
#define LV_COLOR_MIX_ROUND_OFS 0
#define LV_COLOR_CHROMA_KEY lv_color_hex(0x00ff00)

/*=========================
   MEMORY SETTINGS
 *=========================*/
/* Give LVGL a compile-time static DRAM pool – verified correct on ESP32-S3.
 *
 * History of approaches tried:
 *
 * ❌ LV_MEM_CUSTOM=0, static BSS array in DRAM (LV_MEM_SIZE=512 KB):
 *      512 KB overflows .dram0.bss.  BUT current firmware uses only 58 KB
 *      static RAM with 270 KB headroom, so 160 KB fits fine.
 *
 * ❌ LV_MEM_CUSTOM=0, LV_ATTRIBUTE_LARGE_RAM_ARRAY → .ext_ram.bss:
 *      Requires CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY in sdkconfig.
 *      Arduino framework's pre-baked sdkconfig does not have that key.
 *      Workaround: DRAM_ATTR forces .dram1.bss (see below).
 *
 * ❌ LV_MEM_CUSTOM=0, LV_MEM_POOL_ALLOC → heap_caps_malloc(SPIRAM):
 *      LVGL TLSF pool in PSRAM → lv_canvas_set_buffer triggers
 *      lv_img_decoder_get_info; decoder struct in PSRAM gets corrupted by
 *      LVGL TLSF coalescing + OPI cache write-back loss → IllegalInstruction.
 *
 * ❌ LV_MEM_CUSTOM=1, heap_caps_malloc(DRAM):
 *      LVGL alloc/free cycles race with WiFi's concurrent heap use.
 *      The DRAM heap's multi_heap_t.lock field gets overwritten with
 *      0x4FFF4FFF; WiFi's beacon timer calls wifi_malloc which tries
 *      vPortEnterCritical(0x4FFF4FFF) → LoadStoreAlignment (EXCVADDR=
 *      0x4FFF4FFF).  Deterministic: same address every boot.
 *
 * ❌ LV_MEM_CUSTOM=0, LV_MEM_POOL_ALLOC → heap_caps_malloc(DRAM):
 *      One runtime heap_caps_malloc call for the pool.  LVGL's TLSF init
 *      writes a sentinel block at the exact allocation boundary, which
 *      lands on an adjacent heap control struct → same 0x4FFF4FFF crash
 *      (detected via heap_caps_get_free_size returning garbage after
 *      AutopilotScreen::create).
 *
 * ✅ LV_MEM_CUSTOM=0, LV_ATTRIBUTE_LARGE_RAM_ARRAY = DRAM_ATTR (this):
 *      The 160 KB pool is a compile-time static array placed in .dram1.bss
 *      by DRAM_ATTR.  heap_caps_malloc is NEVER called for LVGL.  The DRAM
 *      heap control structures are completely untouched.  WiFi and LVGL
 *      share zero heap infrastructure.
 *      Pool budget: 9 screens' widgets + styles ≈ 60–80 KB.
 *      160 KB gives 2× headroom for draw-ctx temporaries (WindScreen
 *      ~1500 lv_mem_alloc/free per update). */
#define LV_MEM_CUSTOM 0
/* Reduced from 160 KB to 40 KB: our UI uses only 28 KB (observed peak with
 * 9 screens + nav bar).  The freed ~124 KB of DRAM goes to the WiFi stack
 * which otherwise falls back to OPI PSRAM, triggering ESP32-S3 coherency bugs.
  * Increased from 160 KB to 200 KB: the new nav overlay buttons with rounded
 * 40 KB leaves headroom for dynamic LVGL events and animations.
 * NOTE: The crash in circ_calc_aa4 was NOT OOM – root cause was a dangling
 * lv_timer_t* (fixed in DisplayManager.cpp). Reverted from 200 KB back to 160 KB
 * to restore ~32 KB free DRAM (200 KB left only 7.5 KB which is dangerously low).
 * 2026-06: cut 160 KB → 96 KB. Observed LVGL peak is ~32 KB (≈55 KB worst case
 * with 6 data-grids), so 96 KB keeps ample headroom while returning ~64 KB to
 * internal DRAM. That free DRAM is what the async web server / lwIP needs: with
 * only ~26 KB free, large HTTP responses (the UI page) stalled mid-transfer
 * because pbuf allocation failed. ~90 KB free fixes it. */
#ifndef LV_MEM_SIZE   // the PC simulator overrides this (bigger pool, plenty of RAM)
#if defined(BOARD_PANEL_1024X600)
// +10 KB over the 4" pool, financed 1:1 by a smaller LVGL draw buffer
// (DisplaySetup_7B.cpp: 5 rows instead of 10). The 600-grid render needs
// more transient pool per frame (plus the primed draw-buf cache from
// main.cpp), and the full home-launcher plus render peak exceeded 96.5K -
// measured as lv_mem_buf_get/mask OOM reboots.
#define LV_MEM_SIZE   ((106U * 1024U) + 512U)
#else
#define LV_MEM_SIZE   ((96U * 1024U) + 512U)
#endif
#endif
#define LV_MEM_ADR    0
#define LV_MEMCPY_MEMSET  0

/*====================
   HAL SETTINGS
 *====================*/
/* This macro has TWO jobs, and they want different numbers.
 *   1. it is the initial period of the display refresh timer
 *      (lv_hal_disp.c: lv_timer_create(_lv_disp_refr_timer, ...)), and
 *   2. it is LVGL's ANIMATION timestep - lv_anim.c creates its own timer with
 *      the very same macro, so this one number decides how often every fade,
 *      transition and list scroll is advanced.
 *
 * The old value was 200 ms for every board, justified by the fear that
 * lv_timer_handler() would spin forever whenever a render took longer than the
 * period. That is NOT how the vendored LVGL 8.4.0 behaves. Checked in the
 * library sources under .pio/libdeps:
 *   - _lv_disp_refr_timer() calls lv_timer_pause(tmr) BEFORE it renders
 *     (lv_refr.c). That branch is compiled in because LV_USE_PERF_MONITOR and
 *     LV_USE_MEM_MONITOR are both 0 (see DEBUG SETTINGS at the end of this
 *     file). So the refresh timer fires at most once per invalidation, and it
 *     is lv_obj_invalidate() / lv_obj_set_pos() that resume it.
 *   - the outer do-while in lv_timer_handler() (lv_timer.c) only re-enters
 *     while `timer_created || timer_deleted` made the inner walk break out. It
 *     guards against the timer linked list being mutated under the iterator -
 *     it is not an "elapsed > period, so run it again" retry.
 * A short period therefore cannot loop. It only means LVGL is ALLOWED to
 * refresh more often. What 200 ms actually did was cap the 1024x600 boards at
 * 5 fps and step their animations five times a second, which is exactly why
 * the licence text scrolled in visible jumps.
 *
 * 1024x600 boards: 33 ms. That number is chosen for the ANIMATION timestep -
 * the job we want fast. The DISPLAY refresh interval is set separately at
 * runtime: DisplaySetup_7B.cpp calls lv_timer_set_period() on the refresh
 * timer right after lv_disp_drv_register(). It has to be the more
 * conservative of the two, because one 600-grid render plus its flush takes
 * far longer than 33 ms and refreshing back-to-back would leave core 1 with no
 * idle time at all for AsyncTCP and lwIP. Two jobs, two numbers, one place
 * each to tune them.
 *
 * The 4" board is a released product and keeps 200 ms, unchanged. */
#if defined(BOARD_PANEL_1024X600)
/* animation timestep; the display period is set at runtime, see
 * DisplaySetup_7B.cpp */
#define LV_DISP_DEF_REFR_PERIOD 33
#else
#define LV_DISP_DEF_REFR_PERIOD 200  /* 5 fps – must exceed ~100 ms SPI flush */
#endif
#define LV_INDEV_DEF_READ_PERIOD 30  /* ms */
/* 7B only: let LVGL read millis() directly instead of running a dedicated
 * 4 KB tick task (LVTICK) whose loop does nothing but lv_tick_inc(1). Saves
 * the stack + TCB in internal DRAM and removes a 1 kHz context switch.
 * The 4" board and the simulator keep the task (bit-identical builds). */
#if defined(BOARD_PANEL_1024X600) && !defined(SIMULATOR)
  #define LV_TICK_CUSTOM 1
  #define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
  #define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#else
  /* the simulator (also the 7B one) drives lv_tick_inc() via SDL_GetTicks() */
  #define LV_TICK_CUSTOM 0
#endif
#define LV_DPI_DEF 130

/*=======================
 * FEATURE CONFIGURATION
 *=======================*/
#define LV_DRAW_COMPLEX 1
/* 7B only: cap the SUBDIVIDABLE draw-layer chunk (opacity layers) at 8 KB -
 * the 24 KB default is the size of the ENTIRE free LVGL pool headroom here,
 * and lv_draw_sw_layer_create() in 8.4 lv_memset_00()s an alloc result
 * BEFORE its NULL check. Smaller chunks only cost extra blend passes.
 * NOTE this does NOT make transform_zoom safe: zoomed objects use the
 * NON-subdividable branch, which needs the full transformed refresh strip
 * (~22 KB each) in one piece - measured boot loop, see UI7_CONTENT_ZOOM in
 * BoardConfig_7B.h. */
#if defined(BOARD_PANEL_1024X600)
  #define LV_LAYER_SIMPLE_BUF_SIZE          (8 * 1024)
  #define LV_LAYER_SIMPLE_FALLBACK_BUF_SIZE (2 * 1024)
#endif
#define LV_SHADOW_CACHE_SIZE 0
#define LV_CIRCLE_CACHE_SIZE 4
/* Canvas pixel buffers live in PSRAM (PsramArena).  Keeping an image-cache
 * entry for a PSRAM-backed canvas triggers _lv_img_cache_open() which reads
 * the pixel data through the OPI cache.  On ESP32-S3 this can corrupt the
 * SPIRAM TLSF free-list metadata (OPI cache coherency bug) when done
 * concurrently with WiFi GDMA PSRAM traffic.  Setting the cache to 0
 * disables this path entirely; every draw reads directly from PSRAM. */
#define LV_IMG_CACHE_DEF_SIZE 0
#define LV_GRADIENT_MAX_STOPS 2
#define LV_GRAD_CACHE_DEF_SIZE 0
#define LV_DITHER_GRADIENT 0
#define LV_DISP_ROT_MAX_BUF (10*1024)

/*===================
 *  OBJ EXTRA FEATURES
 *==================*/
#define LV_USE_LARGE_COORD 0

/*==================
 *  FONT USAGE
 *==================*/
// Sizes enabled so the WebUI font-size picker has a useful range (~+50 KB flash).
//
// 7B trims 7 sizes that no config has ever selected (not the shipped defaults,
// not the live device snapshot, not any hardcoded screen): 10,18,20,22,28,36,44.
// That is ~267 KB of glyph data - and on the 7B, SPIRAM_RODATA copies all of
// .flash.rodata into PSRAM at boot, so the trim buys back real PSRAM (fonts
// were 73% of rodata). Removal is safe by construction: montserratBySize()
// snaps to the nearest remaining size, so a config asking for 22 gets 24.
// The 4" board and the simulator keep the full ladder (bit-identical builds);
// the WebUI dropdown deliberately keeps offering all sizes for the 4".
#if defined(BOARD_PANEL_1024X600)
  #define LV_MONT_OPT 0   // the never-selected sizes
#else
  #define LV_MONT_OPT 1
#endif
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 LV_MONT_OPT
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
// 18/20/28 are back to ALWAYS-on since the stage-3 font pass: the 7B scales
// every role by UI_S (14->18, 16->20, 24->28 via applyThemeFromConfig), so
// these sizes ARE selected there now. 10/22/36/44 stay trimmed.
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 LV_MONT_OPT
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 LV_MONT_OPT
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 LV_MONT_OPT
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_MONTSERRAT_SUBPX 1
#define LV_FONT_MONTSERRAT_12_SUBPX 0
#define LV_FONT_UNSCII_8  0
#define LV_FONT_UNSCII_16 0

#define LV_FONT_DEFAULT &lv_font_montserrat_16

#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_SUBPX     1
#define LV_FONT_SUBPX_BGR     0
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_FONT_LOADER_FS  0

/*====================
 *  TEXT SETTINGS
 *====================*/
#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS " ,.;:-_"
#define LV_TXT_LINE_BREAK_LONG_LEN 0
#define LV_TXT_LINE_BREAK_LONG_PRE_MIN_LEN  3
#define LV_TXT_LINE_BREAK_LONG_POST_MIN_LEN 3
#define LV_TXT_COLOR_CMD "#"
#define LV_USE_BIDI 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

/*===================
 *  WIDGET USAGE
 *===================*/
#define LV_USE_ARC        1
#define LV_USE_BAR        1
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  1
#define LV_USE_CANVAS     1
#define LV_USE_CHECKBOX   1
#define LV_USE_DROPDOWN   1
#define LV_USE_IMG        1
#define LV_USE_LABEL      1
#define LV_USE_LINE       1
#define LV_USE_ROLLER     1
#define LV_USE_SLIDER     1
#define LV_USE_SWITCH     1
#define LV_USE_TEXTAREA   1
#define LV_USE_TABLE      1
#define LV_USE_CHART      1
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN     0
#define LV_USE_KEYBOARD   1
#define LV_USE_LED        1
#define LV_USE_LIST       1
#define LV_USE_MENU       0
#define LV_USE_METER      1
#define LV_USE_MSGBOX     1
#define LV_USE_SPINBOX    0
#define LV_USE_SPINNER    1
#define LV_USE_TABVIEW    1
#define LV_USE_TILEVIEW   1
#define LV_USE_WIN        0
#define LV_USE_SPAN       0

/*==================
 * THEMES
 *==================*/
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 0
#define LV_THEME_DEFAULT_TRANSITION_TIME 80
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO  0

/*====================
 * LAYOUT
 *====================*/
#define LV_USE_FLEX  1
#define LV_USE_GRID  1

/*=======================
 * 3RD PARTY LIBS
 *=======================*/
#define LV_USE_FS_STDIO 0
#define LV_USE_FS_POSIX 0
#define LV_USE_FS_WIN32 0
#define LV_USE_FS_FATFS 0
#define LV_USE_PNG      0
#define LV_USE_BMP      0
#define LV_USE_SJPG     0
#define LV_USE_GIF      0
#define LV_USE_QRCODE   1   /* WiFi-Hotspot QR in the on-screen config overlay */
#define LV_USE_FREETYPE 0
#define LV_USE_RLOTTIE  0
#define LV_USE_FFMPEG   0

/*==================
 *  OTHERS
 *==================*/
#define LV_USE_SNAPSHOT    0
#define LV_USE_MONKEY      0
#define LV_USE_GRIDNAV     0
#define LV_USE_FRAGMENT    0
#define LV_USE_IMGFONT     0
#define LV_USE_MSG         0
#define LV_USE_IME_PINYIN  0

/*=====================
 *  COMPILER SETTINGS
 *=====================*/
#define LV_BIG_ENDIAN_SYSTEM 0
#define LV_ATTRIBUTE_TICK_INC
#define LV_ATTRIBUTE_TIMER_HANDLER
#define LV_ATTRIBUTE_FLUSH_READY
#define LV_ATTRIBUTE_MEM_ALIGN_SIZE 4
#define LV_ATTRIBUTE_MEM_ALIGN      __attribute__((aligned(4)))
#define LV_ATTRIBUTE_LARGE_CONST
#define LV_ATTRIBUTE_LARGE_RAM_ARRAY DRAM_ATTR
/* FAST_MEM is a function-PLACEMENT hint - it decides where LVGL's hot draw
 * code (blend, mask, line, label, img) LIVES. It has nothing to do with the
 * line directly above, which places the memory POOL and must stay exactly as
 * it is (five failed attempts and their crashes are documented at the top of
 * this file). Changing one does not affect the other.
 *
 * On the ESP32-S3 IRAM and DRAM are carved out of the same block: the linker
 * opens a .dram0.dummy hole exactly as large as the IRAM overhang, so every
 * byte put in IRAM costs one byte of DRAM. On the 1024x600 boards those hot
 * functions are ~12 KB, i.e. IRAM_ATTR there quietly spends ~12 KB of the
 * internal DRAM that lwIP and the async web server live on - and on the 5"
 * board only ~18 KB of it is free, which is why HTTP intermittently stops
 * answering. Dropping the attribute hands those 12 KB straight back.
 *
 * Safe because of two independent things:
 *  * LVGL never runs from an interrupt handler - only from lv_timer_handler()
 *    in the main loop - so none of this code has to survive a window with the
 *    flash cache switched off in the first place.
 *  * The build sets CONFIG_SPIRAM_XIP_FROM_PSRAM=y (custom_sdkconfig in
 *    platformio.ini, the RGB-drift fix), which relocates code out of flash
 *    into PSRAM, so it stays fetchable even while the flash cache IS off.
 *
 * The cost is bandwidth, not correctness: the hot loops now fetch their
 * instructions over the PSRAM bus, the one contended resource here (LCD
 * scan-out reads the frame buffer from it continuously). Two reasons that is
 * affordable: the loops are small and stay in the instruction cache after the
 * first miss, and the display refresh interval on these boards is deliberately
 * left longer than one render (set at runtime in DisplaySetup_7B.cpp, see the
 * LV_DISP_DEF_REFR_PERIOD block above), so there is headroom to spend.
 *
 * The 4" board is a released product and keeps IRAM_ATTR unchanged. Both
 * branches DEFINE the macro, so the "#ifndef LV_ATTRIBUTE_FAST_MEM" fallback
 * in the SIMULATOR block further down still never fires - and on the PC the
 * empty definition simply agrees with the one sim/simulator_defines.h already
 * force-includes. */
#if defined(BOARD_PANEL_1024X600)
#define LV_ATTRIBUTE_FAST_MEM
#else
#define LV_ATTRIBUTE_FAST_MEM       IRAM_ATTR
#endif
#define LV_ATTRIBUTE_DMA
#define LV_EXPORT_CONST_INT(int_value) struct _silence_gcc_warning
#define LV_USE_LARGE_COORD 0

/*===================
 *  DEBUG SETTINGS
 *==================*/
#define LV_USE_LOG  0
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

/* Custom assert handler:
 * - ESP32: esp_restart() (fast, WDT-safe)
 * - Simulator / PC: abort() (shows crash in terminal) */
#ifdef SIMULATOR
#define LV_ASSERT_HANDLER_INCLUDE <stdlib.h>
#define LV_ASSERT_HANDLER abort();
/* Neutralise ESP32-specific function/data placement attributes on PC.
 * Without this, MinGW fails to parse declarations that use these macros. */
#ifndef LV_ATTRIBUTE_FAST_MEM
#define LV_ATTRIBUTE_FAST_MEM
#endif
#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif
#ifndef LV_ATTRIBUTE_LARGE_CONST
#define LV_ATTRIBUTE_LARGE_CONST
#endif
#ifndef LV_ATTRIBUTE_LARGE_RAM_ARRAY
#define LV_ATTRIBUTE_LARGE_RAM_ARRAY
#endif
#else
/* Print the assert site BEFORE restarting - a bare esp_restart() made every
 * LVGL assert look like a silent spontaneous reboot (menu-crash hunt 2026-08). */
#define LV_ASSERT_HANDLER_INCLUDE <esp_system.h>
#define LV_ASSERT_HANDLER do { printf("[lv-assert] %s:%d\n", __FILE__, __LINE__); esp_restart(); } while (0);
#endif

#define LV_LOG_PRINTF 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0
#define LV_USE_REFR_DEBUG   0

#endif /* LV_CONF_H */
#endif /* Enable file content */
