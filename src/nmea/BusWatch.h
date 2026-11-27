#pragma once
// ============================================================
// BusWatch - stale bus values go blank, a silent bus says so.
//
// Two rules, both clocked from DisplayManager::update() under data.lock()
// (the block that also ticks WindPipeline and ApPipeline), so they run on the
// bus, in demo mode and in the PC simulator alike:
//
//  1. Every bus value expires on its own. A value whose sender has not
//     delivered a VALID reading for VALUE_STALE_MS (10 s, the wind's rule) is
//     set to "not known" (NaN / inactive), so the screens show "--" instead of
//     the last reading. Slow senders get SLOW_STALE_MS (environment, distance
//     log, tanks, batteries, the stereo), the magnetic variation gets
//     VARIATION_STALE_MS, AIS targets keep their 5-minute rule (they report
//     that rarely at anchor). Wind and the pilot mode already had this; their
//     own pipelines keep doing it.
//
//  2. The bus as a whole. When NO data value at all has arrived for
//     BUS_SILENT_MS, the state goes SILENT (or NEVER, when nothing came since
//     boot), every bus value is blanked at once - not only the fast ones - and
//     DisplayManager shows a warning strip. Network management does not count
//     as data: heartbeats, address claims, the anchor sync of other displays,
//     the update protocol, system time and the stereo keep a bus "alive" while
//     every instrument is off, which is exactly the case to report.
//
// Kept on purpose: the clock (it runs on from the last time fix, which is
// correct), the tide forecast from the internet, and the history rings
// (echogram, wind rose, pressure trend) - they are history, labelled as such.
//
// NOT an alarm: it never feeds DisplayManager::alarmActive(), which would
// refuse updates over NMEA 2000 - on a bench the update sender is often the
// only node, i.e. a silent bus.
// ============================================================
#include <stdint.h>

class DataModel;

namespace BusWatch {

constexpr uint32_t VALUE_STALE_MS     = 10000;    // per value, the default
constexpr uint32_t SLOW_STALE_MS      = 30000;    // env, log, tanks, batteries, stereo
constexpr uint32_t VARIATION_STALE_MS = 600000;   // changes over years, sent rarely
constexpr uint32_t AIS_STALE_MS       = 300000;   // AIS class A reports every 3 min at anchor
constexpr uint32_t BUS_SILENT_MS      = 10000;    // no data value at all for this long
constexpr uint32_t BOOT_GRACE_MS      = 10000;    // no warning before this, after start

enum class State : uint8_t {
    Starting,   // since start, nothing yet, still inside BOOT_GRACE_MS
    Ok,         // data within BUS_SILENT_MS
    Silent,     // there was data, none for BUS_SILENT_MS
    Never,      // nothing at all since start, BOOT_GRACE_MS over
};

// What changed in one tick - logged by the caller AFTER it has released the
// lock (a Serial write under the data lock holds up the N2K task).
struct Change {
    bool     changed = false;
    State    from = State::Starting, to = State::Starting;
    uint32_t quietMs = 0;    // how long there had been no data (to Ok: the gap)
    const char *what = nullptr;   // to Ok: the value that arrived (string literal)
};

// Call with data.lock() held, nowMs read under that lock. Expires stale
// values, then updates the bus state; entering Silent/Never blanks every bus
// value. Cheap: a few dozen compares (the AIS table is swept once a second).
Change tick(DataModel &d, uint32_t nowMs);

// Readable from any task without the lock (a single byte).
State state();
bool  silent();            // Silent or Never - the warning is up
const char *stateKey(State s);   // "starting" / "ok" / "silent" / "never" (/api/data)
// How long there has been no data: since the newest data value arrived
// (Silent), since the first tick (Never), 0 while data flows. No lock needed.
uint32_t quietMs(uint32_t nowMs);

// Lock held. The last time any data value arrived (0 = never), and if asked,
// which value that was (a string literal, for the log); and whether own
// position is current (the anchor watch).
uint32_t lastDataMs(const DataModel &d, const char **which = nullptr);
bool     positionFresh(const DataModel &d, uint32_t nowMs);

// Signed age, so a stamp written a moment after the reader took its clock
// (other core) counts as fresh instead of wrapping to ~49 days; 0 = never.
inline bool fresh(uint32_t stamp, uint32_t nowMs, uint32_t maxAgeMs) {
    return stamp != 0 && (int32_t)(nowMs - stamp) < (int32_t)maxAgeMs;
}

} // namespace BusWatch
