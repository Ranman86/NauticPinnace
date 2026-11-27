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

#define FW_VERSION      "1.3.0"

// Model version reported alongside it on the bus. This describes the HARDWARE
// generation, not the software, and therefore moves only when a new board
// variant ships - not with every firmware release.
#define FW_MODEL_VERSION "1.1"

// Which board this binary was built for. Three variants share these sources
// and produce visibly different firmware, so "which version" is only half the
// question when someone reports a problem - GET /api/info answers both.
//
// EXACTLY ONE board flag per firmware env (docs/N2K-UPDATE.md 8.1). This used
// to fall back to the 4-inch for any env without a 7B or 5B flag, so a build
// that forgot its flag silently became a "4-inch" image - harmless while the
// name was only logged, dangerous now that it goes into the firmware
// descriptor that an update over NMEA 2000 checks before it writes anything.
// The simulator is the one build without a board flag (or with the 7B flag,
// for the 1024x600 layout); it carries no descriptor.
#if !defined(SIMULATOR) && \
    (defined(BOARD_WAVESHARE_4) + defined(BOARD_WAVESHARE_7B) + defined(BOARD_WAVESHARE_5B)) != 1
#error "exactly one of BOARD_WAVESHARE_4 / _7B / _5B must be defined"
#endif

// NP_BOARD_ID: the board id of the update protocol (docs/N2K-UPDATE.md 4.1).
// Only ever added to, never renumbered: senders compare it with the id in
// every image they offer.
#if defined(BOARD_WAVESHARE_7B)
#define FW_BOARD_NAME "waveshare-7b-1024x600"
#define NP_BOARD_ID   2
#elif defined(BOARD_WAVESHARE_5B)
#define FW_BOARD_NAME "waveshare-5b-1024x600"
#define NP_BOARD_ID   3
#elif defined(SIMULATOR)
#define FW_BOARD_NAME "simulator"
#define NP_BOARD_ID   0
#else
#define FW_BOARD_NAME "waveshare-4inch-480x480"
#define NP_BOARD_ID   1
#endif
