#pragma once
// ============================================================
// BoardConfig.h – board selector
//
// Every other file keeps including "BoardConfig.h"; which pin map it actually
// gets is decided here, from the build flag set by the PlatformIO env:
//
//   env:waveshare_esp32s3_4   -> (no flag)           -> BoardConfig_4.h
//   env:waveshare_esp32s3_7b  -> -DBOARD_WAVESHARE_7B -> BoardConfig_7B.h
//   env:waveshare_esp32s3_5b  -> -DBOARD_WAVESHARE_5B -> BoardConfig_5B.h
//
// The 4" board stays the default so an unflagged or simulator build behaves
// exactly as before.
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
#else
  #include "BoardConfig_4.h"
#endif
