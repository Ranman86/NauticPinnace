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
//
// The scenario above is TRUE wind, but only the APPARENT wind it
// gives at the demo's STW is handed to the model - through
// WindPipeline, exactly like a masthead unit on the bus - so the
// display computes and damps true wind and TWD itself in demo mode
// too.
// ============================================================
class DemoDataSource {
public:
    // Call from the N2K task loop (rate-limited internally to ~5 Hz)
    void tick();
};

extern DemoDataSource demoData;

#ifdef SIMULATOR
// ── Simulator test harness (sim/sim_main.cpp) ─────────────────────────────
// --freeze-demo T: the demo's time base. NAN (the default) = seconds since
// start, as on the device; any other value is used as t in its place, so
// every demo value becomes a pure function of T. The freshness stamps keep
// the real millis(), so nothing ages out while the scenario stands still.
extern float g_simFreezeT;
// Called at the end of tick(), still under the data lock, with the tick's
// real stamp and the time base it used. This is where --set and --ais-seed
// lay their values over the demo's: inside the same lock, no screen can read
// the demo's own rudder angle or date in between. nullptr = no override.
extern void (*g_simDemoOverride)(uint32_t nowMs, float t);
#endif
