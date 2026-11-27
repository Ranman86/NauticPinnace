// ============================================================================
// ApPipeline.cpp - see ApPipeline.h.
// ============================================================================
#include "ApPipeline.h"
#include "DataModel.h"
#if !defined(SIMULATOR)
#include <Preferences.h>
#endif

namespace {

ApDecode::NameTable s_names;   // bus task only

// One log line per address whose frames are dropped for want of a NAME -
// the symptom of the limitation described at ApDecode::NameTable.
uint8_t s_warnedNoName[32];
// One log line per run for a wind setpoint the configured encoding cannot
// produce (see ApDecode::WIND_SETPOINT_SIGNED).
bool s_warnedWindRange = false;

const float RAD2DEG_F = 57.2957795131f;

// ---- The remembered pilot computer (bus task only) --------------------------
struct Remembered { uint64_t name; uint8_t sa; bool valid; };
Remembered s_mem = { 0, 0xFF, false };      // what is stored, or about to be
enum { PERSIST_NONE = 0, PERSIST_STORE, PERSIST_ERASE };
uint8_t s_persist = PERSIST_NONE;           // done by persistDue(), outside the lock

void packRemembered(const Remembered &r, uint8_t b[9]) {
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(r.name >> (8 * i));
    b[8] = r.sa;
}
void unpackRemembered(const uint8_t b[9], Remembered &r) {
    r.name = 0;
    for (int i = 7; i >= 0; i--) r.name = (r.name << 8) | b[i];
    r.sa    = b[8];
    r.valid = true;
}

#if !defined(SIMULATOR)
// NVS, not the config file: it survives "uploadfs" and a factory config, and
// it is not a setting anybody should edit.
const char *NVS_NS  = "ap";
const char *NVS_KEY = "pilot";        // 8 bytes NAME (LE) + 1 byte address

bool loadRemembered(Remembered &r) {
    Preferences p;
    if (!p.begin(NVS_NS, true)) return false;      // read-only; absent on first boot
    uint8_t b[9];
    const bool ok = p.getBytesLength(NVS_KEY) == sizeof(b) &&
                    p.getBytes(NVS_KEY, b, sizeof(b)) == sizeof(b);
    p.end();
    if (ok) unpackRemembered(b, r);
    return ok;
}
void storeRemembered(const Remembered *r) {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    if (r) {
        uint8_t b[9];
        packRemembered(*r, b);
        p.putBytes(NVS_KEY, b, sizeof(b));
    } else if (p.isKey(NVS_KEY)) {
        p.remove(NVS_KEY);
    }
    p.end();
}
#else
// The simulator keeps it in memory: enough for --ap-frames to restart the
// display (resetNames() + begin()) and find the pilot again.
uint8_t s_simStore[9];
bool    s_simStored = false;
bool loadRemembered(Remembered &r) {
    if (s_simStored) unpackRemembered(s_simStore, r);
    return s_simStored;
}
void storeRemembered(const Remembered *r) {
    s_simStored = r != nullptr;
    if (r) packRemembered(*r, s_simStore);
}
#endif

void forgetRemembered(const char *why) {
    Serial.printf("[ap] forgetting the remembered pilot computer at SA %u: %s\n",
                  (unsigned)s_mem.sa, why);
    s_mem.valid = false;
    s_persist   = PERSIST_ERASE;
}

float toDeg(float rad, ApDecode::SetpointKind kind) {
    if (isnan(rad)) return NAN;
    float d = rad * RAD2DEG_F;
    if (kind == ApDecode::SP_HEADING) {
        while (d >= 360.f) d -= 360.f;
        while (d <    0.f) d += 360.f;
    }
    return d;
}

// Copy the tracker's verdict into the fields the screens and /api/data read.
void publish(uint32_t nowMs) {
    const ApDecode::Tracker::View v = data.apTrack.view(nowMs);
    const uint8_t before = data.apMode;
    data.apMode           = v.mode;
    data.apEngaged        = v.engaged;
    data.apModeSource     = v.source;
    data.apPilotSa        = v.sa;
    data.lastApModeMs     = v.modeMs;
    data.apStatus         = v.status;
    data.apSetpointKind   = v.spKind;
    data.apSetpoint       = toDeg(v.spRad, v.spKind);
    data.lastApSetpointMs = v.spMs;
    if (before == v.mode || v.source == ApDecode::SRC_DEMO) return;
    if (v.source == ApDecode::SRC_NONE)
        Serial.printf("[ap] mode %s -> unknown (no status for 10 s)\n",
                      ApDecode::modeKey((ApDecode::Mode)before));
    else
        Serial.printf("[ap] mode %s -> %s (PGN %s, SA %u)\n",
                      ApDecode::modeKey((ApDecode::Mode)before), ApDecode::modeKey(v.mode),
                      v.source == ApDecode::SRC_MODE_REPORT ? "65305" : "65340",
                      (unsigned)v.sa);
}

// The pilot computer whose frames decide the mode is the one to remember.
void rememberDeciding() {
    if (data.apModeSource != ApDecode::SRC_MODE_REPORT &&
        data.apModeSource != ApDecode::SRC_STATE) return;
    const uint8_t sa = data.apPilotSa;
    uint64_t name;
    if (sa >= 254 || !s_names.lookup(sa, name) || !ApDecode::isPilotComputer(name)) return;
    if (s_mem.valid && s_mem.name == name && s_mem.sa == sa) return;
    s_mem.name  = name;
    s_mem.sa    = sa;
    s_mem.valid = true;
    s_persist   = PERSIST_STORE;
    Serial.printf("[ap] remembering the pilot computer at SA %u for the next start\n",
                  (unsigned)sa);
}

} // namespace

void ApPipeline::begin() {
    Remembered r;
    if (!loadRemembered(r)) return;
    if (r.sa >= 254 || !ApDecode::isPilotComputer(r.name)) return;
    uint64_t heard;
    if (s_names.lookup(r.sa, heard) && heard != r.name) return;   // a claim heard since wins
    s_mem = r;
    s_names.claimName(r.sa, r.name);
    Serial.printf("[ap] pilot computer remembered at SA %u from the last run - "
                  "used until a claim on the bus says otherwise\n", (unsigned)r.sa);
}

void ApPipeline::addressClaim(uint8_t sa, const uint8_t *d, uint8_t len) {
    uint64_t name;
    if (!ApDecode::nameFromClaim(d, len, name)) return;
    if (sa == 255) return;                          // never a source address
    if (ApDecode::isPilotComputer(name)) {
        if (sa == 254) {
            Serial.printf("[ap] pilot computer cannot claim an address\n");
        } else {
            uint64_t old = 0;
            if (!(s_names.lookup(sa, old) && old == name))
                Serial.printf("[ap] pilot computer claimed SA %u\n", (unsigned)sa);
            s_warnedNoName[sa >> 3] &= (uint8_t)~(1u << (sa & 7));
        }
    }
    // The remembered pilot follows its NAME and gives way to any other one.
    if (s_mem.valid) {
        if (name == s_mem.name && sa != s_mem.sa) {
            if (sa == 254) {
                forgetRemembered("it cannot claim an address");
            } else {
                Serial.printf("[ap] remembered pilot computer moved to SA %u\n", (unsigned)sa);
                s_mem.sa  = sa;
                s_persist = PERSIST_STORE;
            }
        } else if (name != s_mem.name && sa == s_mem.sa) {
            forgetRemembered("another device claimed that address");
        }
    }
    s_names.claim(sa, d, len);
}

bool ApPipeline::isPilotComputer(uint8_t sa) {
    uint64_t name;
    return s_names.lookup(sa, name) && ApDecode::isPilotComputer(name);
}

void ApPipeline::persistDue() {
    if (s_persist == PERSIST_NONE) return;
    const uint8_t what = s_persist;
    s_persist = PERSIST_NONE;
    storeRemembered(what == PERSIST_STORE && s_mem.valid ? &s_mem : nullptr);
}

void ApPipeline::resetNames() {
    s_names.reset();
    memset(s_warnedNoName, 0, sizeof(s_warnedNoName));
}

ApDecode::Facts ApPipeline::frame(uint32_t pgn, uint8_t sa, const uint8_t *d,
                                  uint8_t len, uint32_t nowMs) {
    uint64_t name = 0;
    const bool known = s_names.lookup(sa, name);
    const ApDecode::Facts f = ApDecode::decode(pgn, sa, d, len, known, name);
    if (f.ignored == ApDecode::IGN_NAME_UNKNOWN &&
        !(s_warnedNoName[sa >> 3] & (1u << (sa & 7)))) {
        s_warnedNoName[sa >> 3] |= (uint8_t)(1u << (sa & 7));
        Serial.printf("[ap] PGN %lu from SA %u ignored: no address claim seen from it yet "
                      "(the mode stays \"--\" until it claims again)\n",
                      (unsigned long)pgn, (unsigned)sa);
    }
    if (f.setpointOutOfRange && !s_warnedWindRange) {
        s_warnedWindRange = true;
        Serial.printf("[ap] PGN 65341 from SA %u: wind setpoint raw %u is above 2 pi - it looks "
                      "signed; see WIND_SETPOINT_SIGNED in ApDecode.h\n",
                      (unsigned)sa, (unsigned)f.setpointRaw);
    }
    data.apTrack.feed(f, nowMs);
    publish(nowMs);
    rememberDeciding();
    return f;
}

void ApPipeline::demo(ApDecode::Mode mode, ApDecode::Status status,
                      ApDecode::SetpointKind kind, float setpointDeg, uint32_t nowMs) {
    data.apTrack.demo(mode, status, kind,
                      isnan(setpointDeg) ? NAN : setpointDeg / RAD2DEG_F, nowMs);
    publish(nowMs);
}

void ApPipeline::tick(uint32_t nowMs) {
    data.apTrack.expire(nowMs);
    publish(nowMs);
}
