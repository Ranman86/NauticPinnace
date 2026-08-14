#pragma once
#include <Arduino.h>

// ============================================================
// DemoDataSource – generates synthetic, animated sailing data
// and pushes it into DataModel at ~5 Hz.
//
// Scenario (8-minute cycle):
//   0 – 240 s  : Starboard tack  TWA +20° → +165° → +20°
//   240 – 480 s : Port tack      TWA −20° → −165° → −20°
//
// The sweep passes through every point-of-sail zone:
//   No-Go (flutter), Am Wind, Halb-am-Wind, Halbwind,
//   Raumwind, Vorwind (spinnaker threshold).
//
// Wind speed oscillates 8 – 18 kn (period ~150 s).
// Hull-speed model determines reef hints.
// ============================================================
class DemoDataSource {
public:
    // Call from the N2K task loop (rate-limited internally to ~5 Hz)
    void tick();
};

extern DemoDataSource demoData;
