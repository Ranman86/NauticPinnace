#pragma once
// ============================================================
// BoardConfig.h – board selector
//
// Every other file keeps including "BoardConfig.h"; which pin map it actually
// gets is decided here, from the build flag set by the PlatformIO env:
//
//   env:waveshare_esp32s3_4   -> -DBOARD_WAVESHARE_4  -> BoardConfig_4.h
//   env:waveshare_esp32s3_7b  -> -DBOARD_WAVESHARE_7B -> BoardConfig_7B.h
//   env:waveshare_esp32s3_5b  -> -DBOARD_WAVESHARE_5B -> BoardConfig_5B.h
//
// Every firmware env sets exactly one board flag, and Version.h stops the
// build otherwise. The unflagged fallback to the 4" pin map exists for the
// PC simulator only: a firmware build that forgot its flag must fail here,
// not quietly become a 4-inch image (docs/N2K-UPDATE.md 8.1).
//
// NOTE the second, orthogonal macro: -DBOARD_PANEL_1024X600 (set by BOTH the
// 7B and 5B envs, see platformio.ini). Everything about the 1024x600 USER
// INTERFACE - 600-grid screens, nav rail, data sidebar, home launcher, font
// ladder, arena and LVGL pool sizes - keys off THAT flag, never off the board
// identity. A further 1024x600 board therefore only needs a pin header plus
// the two flags; no UI code has to learn about it.
// ============================================================

#if defined(BOARD_WAVESHARE_7B)
  #include "BoardConfig_7B.h"
#elif defined(BOARD_WAVESHARE_5B)
  #include "BoardConfig_5B.h"
#elif defined(BOARD_WAVESHARE_4) || defined(SIMULATOR)
  #include "BoardConfig_4.h"
#else
  #error "no board flag: set BOARD_WAVESHARE_4, _7B or _5B in the env's build_flags"
#endif
