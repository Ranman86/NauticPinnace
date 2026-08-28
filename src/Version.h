#pragma once

// ============================================================================
// Firmware version - the ONE place it is written down.
//
// It used to live in two places that had already drifted apart: the NMEA 2000
// product information said "1.0.0" while the boot screen said "v1.0". The bus
// value is the one that matters most, because it is what a chartplotter shows
// in its device list - so on a boat with several of these displays, that list
// is the only way to tell which one is running what. It has to be true.
//
// WHEN TO BUMP: any time firmware is flashed onto a device that leaves the
// bench. An unchanged version on changed firmware is worse than no version at
// all, because the plotter then states something false with confidence.
//
// FORMAT: MAJOR.MINOR.PATCH.
//   MAJOR  incompatible change to stored configuration or bus behaviour
//   MINOR  new screens, new hardware support, new features
//   PATCH  fixes and performance work only
//
// The NMEA 2000 product information field is limited by the standard, so keep
// this short - "1.1.0" or "1.1.0-rc1" fit, a git hash does not.
// ============================================================================

#define FW_VERSION      "1.1.0"

// Model version reported alongside it on the bus. This describes the HARDWARE
// generation, not the software, and therefore moves only when a new board
// variant ships - not with every firmware release.
#define FW_MODEL_VERSION "1.1"

// Which board this binary was built for. Three variants share these sources
// and produce visibly different firmware, so "which version" is only half the
// question when someone reports a problem - GET /api/info answers both.
#if defined(BOARD_WAVESHARE_7B)
#define FW_BOARD_NAME "waveshare-7b-1024x600"
#elif defined(BOARD_WAVESHARE_5B)
#define FW_BOARD_NAME "waveshare-5b-1024x600"
#elif defined(SIMULATOR)
#define FW_BOARD_NAME "simulator"
#else
#define FW_BOARD_NAME "waveshare-4inch-480x480"
#endif
