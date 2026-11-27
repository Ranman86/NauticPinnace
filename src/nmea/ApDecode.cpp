// ============================================================================
// ApDecode.cpp - see ApDecode.h. Pure C++11, host-tested.
// ============================================================================
#include "ApDecode.h"

namespace ApDecode {

namespace {

const float TWO_PI_F = 6.28318530718f;
const float PI_F     = 3.14159265359f;
// 2 pi in 0.0001 rad, rounded: the largest raw value an unsigned 0..2 pi
// angle can have.
const uint16_t RAW_TWO_PI = 62832;

inline uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// The proprietary header, byte for byte: "41 9F". Manufacturer code and
// industry group alone are not enough - the reserved bits must be set too.
inline bool headerOk(const uint8_t *d) {
    return d[0] == PROP_HEADER_B0 && d[1] == PROP_HEADER_B1;
}

// 65341 byte 4 -> mode and the kind of setpoint it holds.
Mode setpointModeOf(uint8_t b, SetpointKind &kind) {
    kind = SP_NONE;
    switch (b) {
        case 2:  kind = SP_HEADING; return MODE_AUTO;
        case 3:  kind = SP_WIND;    return MODE_WIND;
        case 10: kind = SP_HEADING; return MODE_NAV;
        // NoDrift holds a course, so its setpoint is taken as a heading.
        case 11: kind = SP_HEADING; return MODE_NO_DRIFT;
        // Any other value: neither its meaning nor what bytes 6..7 hold then
        // is known. No setpoint, and as a mode "engaged, not known" - never a
        // guess (information only: 65341 does not decide the mode).
        default: return MODE_ACTIVE;
    }
}

// THE place a raw wind setpoint becomes an angle - see WIND_SETPOINT_SIGNED
// and WIND_SETPOINT_STBD_LOW. Returns NaN for a value the encoding cannot
// produce (and says so in outOfRange).
float windFromRaw(uint16_t raw, bool &outOfRange) {
    outOfRange = false;
    float rad;
    if (WIND_SETPOINT_SIGNED) {
        rad = (float)(int16_t)raw * 0.0001f;
        if (rad >  PI_F) rad -= TWO_PI_F;      // int16 reaches +-3.2767 rad
        if (rad < -PI_F) rad += TWO_PI_F;
    } else {
        if (raw > RAW_TWO_PI) { outOfRange = true; return NAN; }
        rad = raw * 0.0001f;
        if (rad > PI_F) rad -= TWO_PI_F;       // pi..2 pi -> the other side
    }
    return WIND_SETPOINT_STBD_LOW ? rad : -rad;
}

} // namespace

SetpointKind setpointKindFor(Mode m) {
    switch (m) {
        case MODE_AUTO:
        case MODE_NAV:
        case MODE_NO_DRIFT:  return SP_HEADING;
        case MODE_WIND:      return SP_WIND;
        default:             return SP_NONE;
    }
}

const char *modeKey(Mode m) {
    switch (m) {
        case MODE_STANDBY:  return "standby";
        case MODE_AUTO:     return "auto";
        case MODE_WIND:     return "wind";
        case MODE_NAV:      return "nav";
        case MODE_NO_DRIFT: return "no_drift";
        case MODE_ACTIVE:   return "active";
        default:            return "unknown";
    }
}

// ---- NAME -------------------------------------------------------------------

bool nameFromClaim(const uint8_t *d, uint8_t len, uint64_t &name) {
    if (!d || len < 8) return false;
    name = 0;
    for (int i = 7; i >= 0; i--) name = (name << 8) | d[i];
    return true;
}

bool isPilotComputer(uint64_t name) {
    return nameClass(name) == PILOT_CLASS && nameFunction(name) == PILOT_FUNCTION &&
           nameManufacturer(name) == MFR_CODE;
}

void NameTable::reset() {
    for (int i = 0; i < CAPACITY; i++) { e[i].name = 0; e[i].sa = 0xFF; e[i].used = false; }
}

bool NameTable::claim(uint8_t sa, const uint8_t *d, uint8_t len) {
    uint64_t name;
    if (!nameFromClaim(d, len, name)) return false;
    if (sa == 255) return false;            // the global address is never a source
    claimName(sa, name);
    return true;
}

void NameTable::claimName(uint8_t sa, uint64_t name) {
    // The NAME leaves any address it held before. For sa 254 ("cannot
    // claim") that is all there is to do.
    for (int i = 0; i < CAPACITY; i++)
        if (e[i].used && e[i].name == name && e[i].sa != sa) e[i].used = false;
    if (sa >= 254) return;
    for (int i = 0; i < CAPACITY; i++)
        if (e[i].used && e[i].sa == sa) { e[i].name = name; return; }
    int slot = -1;
    for (int i = 0; i < CAPACITY && slot < 0; i++)
        if (!e[i].used) slot = i;
    // Full: only a pilot computer is worth a slot, and only one that does not
    // hold a pilot computer already.
    if (slot < 0 && isPilotComputer(name))
        for (int i = 0; i < CAPACITY && slot < 0; i++)
            if (!isPilotComputer(e[i].name)) slot = i;
    if (slot < 0) return;
    e[slot].name = name;
    e[slot].sa   = sa;
    e[slot].used = true;
}

bool NameTable::lookup(uint8_t sa, uint64_t &name) const {
    for (int i = 0; i < CAPACITY; i++)
        if (e[i].used && e[i].sa == sa) { name = e[i].name; return true; }
    return false;
}

int NameTable::count() const {
    int n = 0;
    for (int i = 0; i < CAPACITY; i++) if (e[i].used) n++;
    return n;
}

// ---- Frame decoding ---------------------------------------------------------

Facts::Facts()
    : ignored(IGN_NONE), pgn(0), sa(0xFF), model(0xFF), report(0xFF), word(0),
      hasMode(false), mode(MODE_UNKNOWN), engaged(false),
      hasStatus(false), status(STATUS_UNKNOWN),
      hasSetpoint(false), setpointMode(MODE_UNKNOWN), setpointKind(SP_NONE),
      setpointRad(NAN), setpointRaw(0xFFFF), setpointOutOfRange(false) {}

Mode modeFromWord(uint32_t w) {
    if (w & 0x08) return MODE_STANDBY;          // standby, whatever else is set
    switch (w) {
        case 0:     return MODE_UNKNOWN;        // says nothing
        case 0x10:  return MODE_AUTO;
        case 0x40:  return MODE_NAV;
        case 0x100: return MODE_NO_DRIFT;
        case 0x400: return MODE_WIND;
        // Two of these at once, or any bit the rules do not know: engaged,
        // but the mode is not known - shown as such, never guessed.
        default:    return MODE_ACTIVE;
    }
}

Facts decode(uint32_t pgn, uint8_t sa, const uint8_t *d, uint8_t len,
             bool nameKnown, uint64_t name) {
    Facts f;
    f.pgn = pgn;
    f.sa  = sa;
    if (pgn != PGN_MODE_REPORT && pgn != PGN_STATE && pgn != PGN_SETPOINT) {
        f.ignored = IGN_PGN; return f;
    }
    // Header and NAME before anything else: a frame that is not from the
    // pilot computer must not leave a trace in the result.
    if (!d || len < 2)             { f.ignored = IGN_SHORT;        return f; }
    if (!headerOk(d))              { f.ignored = IGN_HEADER;       return f; }
    if (!nameKnown)                { f.ignored = IGN_NAME_UNKNOWN; return f; }
    if (!isPilotComputer(name))    { f.ignored = IGN_NOT_PILOT;    return f; }

    if (pgn == PGN_MODE_REPORT) {
        if (len < 4) { f.ignored = IGN_SHORT; return f; }
        f.model  = d[2];
        f.report = d[3];
        if (f.report == REPORT_MODE) {
            if (len < 8) { f.ignored = IGN_SHORT; return f; }
            f.word = le32(d + 4);
            const Mode m = modeFromWord(f.word);
            if (m == MODE_UNKNOWN) { f.ignored = IGN_UNDECODED; return f; }
            f.hasMode = true;
            f.mode    = m;
            f.engaged = isEngaged(m);
        } else if (f.report == REPORT_STATUS) {
            if (len < 5) { f.ignored = IGN_SHORT; return f; }
            if      (d[4] == 2)  f.status = STATUS_MANUAL;
            else if (d[4] == 16) f.status = STATUS_AUTOMATIC;
            else { f.ignored = IGN_UNDECODED; return f; }
            f.hasStatus = true;
        } else {
            f.ignored = IGN_REPORT;
        }
        return f;
    }

    if (pgn == PGN_STATE) {
        if (len < 6) { f.ignored = IGN_SHORT; return f; }
        if (d[5] == 0xF8) {
            f.mode = MODE_STANDBY;
        } else if (d[5] == 0xFA) {
            switch (d[3]) {
                case 1:  f.mode = MODE_AUTO;   break;
                case 3:  f.mode = MODE_WIND;   break;
                case 4:  f.mode = MODE_NAV;    break;
                default: f.mode = MODE_ACTIVE; break;   // 0 = no sub-mode
            }
        } else {
            f.ignored = IGN_UNDECODED; return f;
        }
        f.hasMode = true;
        f.engaged = isEngaged(f.mode);
        return f;
    }

    // PGN_SETPOINT
    if (len < 8) { f.ignored = IGN_SHORT; return f; }
    SetpointKind kind;
    f.setpointMode = setpointModeOf(d[4], kind);
    f.setpointKind = kind;
    f.hasSetpoint  = true;
    const uint16_t raw = (uint16_t)(d[6] | (d[7] << 8));
    f.setpointRaw = raw;
    if (raw >= 0xFFFD || kind == SP_NONE) return f;           // n/a: setpointRad stays NaN
    if (kind == SP_WIND) {
        f.setpointRad = windFromRaw(raw, f.setpointOutOfRange);
    } else {
        float rad = raw * 0.0001f;
        if (rad >= TWO_PI_F) rad -= TWO_PI_F;                 // a heading is 0..2 pi
        f.setpointRad = rad;
    }
    return f;
}

// ---- Tracker ----------------------------------------------------------------

void Tracker::reset() {
    for (int i = 0; i < CH_N; i++) {
        Chan &c = ch[i];
        c.have = false; c.stable = MODE_UNKNOWN; c.sa = 0xFF;
        c.pend = MODE_UNKNOWN; c.pendSa = 0xFF; c.pendCount = 0;
        c.lastMs = 0;
    }
    status = STATUS_UNKNOWN; statusMs = 0; haveStatus = false;
    spHdg.have  = false; spHdg.rad  = NAN; spHdg.ms  = 0;
    spWind.have = false; spWind.rad = NAN; spWind.ms = 0;
}

bool Tracker::fresh(bool have, uint32_t lastMs, uint32_t nowMs) {
    if (!have) return false;
    // Signed: a stamp taken a moment AFTER nowMs (another task) is fresh,
    // not 49 days old.
    const int32_t age = (int32_t)(nowMs - lastMs);
    return age <= (int32_t)STALE_MS;
}

void Tracker::expire(uint32_t nowMs) {
    for (int i = 0; i < CH_N; i++) {
        Chan &c = ch[i];
        if (c.have && !fresh(true, c.lastMs, nowMs)) {
            c.have = false; c.stable = MODE_UNKNOWN; c.sa = 0xFF;
            c.pend = MODE_UNKNOWN; c.pendSa = 0xFF; c.pendCount = 0;
        }
    }
    if (haveStatus && !fresh(true, statusMs, nowMs)) { haveStatus = false; status = STATUS_UNKNOWN; }
    Sp *sps[2] = { &spHdg, &spWind };
    for (int i = 0; i < 2; i++)
        if (sps[i]->have && !fresh(true, sps[i]->ms, nowMs)) {
            sps[i]->have = false; sps[i]->rad = NAN; sps[i]->ms = 0;
        }
}

void Tracker::feedMode(Chan &c, Mode m, uint8_t sa, uint32_t nowMs) {
    // A standing mode belongs to the address it came from. Another source -
    // a second pilot computer - is not heard on this input until that one
    // has been quiet for STALE_MS and expire() has freed it; it must not
    // keep the input alive either, or a stale mode would stand for ever.
    if (c.stable != MODE_UNKNOWN && sa != c.sa) return;
    // Two identical modes in a row FROM THE SAME ADDRESS.
    if (c.pendCount && c.pend == m && c.pendSa == sa) {
        if (c.pendCount < 255) c.pendCount++;
    } else {
        c.pend = m; c.pendSa = sa; c.pendCount = 1;
    }
    if (c.pendCount >= 2) { c.stable = m; c.sa = sa; }
    c.have   = true;
    c.lastMs = nowMs;
}

int Tracker::deciding(uint32_t nowMs) const {
    // The demo first, then 65305 report 10, then 65340.
    static const int order[CH_N] = { CH_DEMO, CH_MODE_REPORT, CH_STATE };
    for (int i = 0; i < CH_N; i++) {
        const Chan &c = ch[order[i]];
        if (c.stable != MODE_UNKNOWN && fresh(c.have, c.lastMs, nowMs)) return order[i];
    }
    return -1;
}

void Tracker::feed(const Facts &f, uint32_t nowMs) {
    if (f.ignored != IGN_NONE) return;
    expire(nowMs);   // a source back after a silence starts over
    if (f.hasMode) {
        if (f.pgn == PGN_MODE_REPORT) feedMode(ch[CH_MODE_REPORT], f.mode, f.sa, nowMs);
        else if (f.pgn == PGN_STATE)  feedMode(ch[CH_STATE],       f.mode, f.sa, nowMs);
    }
    // Status and setpoints: from the address whose mode is shown, once there
    // is one (the demo has no address and lets the bus through).
    const int dec = deciding(nowMs);
    if (dec >= 0 && ch[dec].sa != 0xFF && f.sa != ch[dec].sa) return;
    if (f.hasStatus) { status = f.status; statusMs = nowMs; haveStatus = true; }
    if (f.hasSetpoint && f.setpointKind != SP_NONE) {
        // Only the kind the mode byte names; "not available" (NaN) clears
        // that kind rather than leaving an old value up.
        Sp &s = f.setpointKind == SP_WIND ? spWind : spHdg;
        s.rad  = f.setpointRad;
        s.ms   = nowMs;
        s.have = !isnan(f.setpointRad);
    }
}

void Tracker::demo(Mode m, Status s, SetpointKind k, float rad, uint32_t nowMs) {
    Chan &c = ch[CH_DEMO];
    c.have = true; c.stable = m; c.sa = 0xFF;
    c.pend = m; c.pendSa = 0xFF; c.pendCount = 2;
    c.lastMs = nowMs;
    status = s; statusMs = nowMs; haveStatus = (s != STATUS_UNKNOWN);
    if (k != SP_NONE) {
        Sp &sp = k == SP_WIND ? spWind : spHdg;
        sp.rad = rad; sp.ms = nowMs; sp.have = !isnan(rad);
    }
}

Tracker::View Tracker::view(uint32_t nowMs) const {
    View v;
    v.mode = MODE_UNKNOWN; v.engaged = false;
    v.source = SRC_NONE; v.sa = 0xFF; v.modeMs = 0;
    const int dec = deciding(nowMs);
    if (dec >= 0) {
        const Chan &c = ch[dec];
        v.mode    = c.stable;
        v.engaged = isEngaged(c.stable);
        v.source  = dec == CH_DEMO ? SRC_DEMO : dec == CH_MODE_REPORT ? SRC_MODE_REPORT : SRC_STATE;
        v.sa      = c.sa;
        v.modeMs  = c.lastMs;
    }
    const bool stOk = fresh(haveStatus, statusMs, nowMs);
    v.status = stOk ? status : STATUS_UNKNOWN;

    const bool hdgOk  = fresh(spHdg.have,  spHdg.ms,  nowMs);
    const bool windOk = fresh(spWind.have, spWind.ms, nowMs);
    v.hdgSpRad  = hdgOk  ? spHdg.rad  : NAN;  v.hdgSpMs  = hdgOk  ? spHdg.ms  : 0;
    v.windSpRad = windOk ? spWind.rad : NAN;  v.windSpMs = windOk ? spWind.ms : 0;

    SetpointKind k = setpointKindFor(v.mode);
    if (v.mode == MODE_UNKNOWN || v.mode == MODE_ACTIVE) {
        // The mode does not say which: the newer one (signed age, as above).
        if (hdgOk && windOk) k = (int32_t)(spWind.ms - spHdg.ms) > 0 ? SP_WIND : SP_HEADING;
        else k = windOk ? SP_WIND : hdgOk ? SP_HEADING : SP_NONE;
    }
    if (k == SP_HEADING && hdgOk)      { v.spKind = SP_HEADING; v.spRad = spHdg.rad;  v.spMs = spHdg.ms; }
    else if (k == SP_WIND && windOk)   { v.spKind = SP_WIND;    v.spRad = spWind.rad; v.spMs = spWind.ms; }
    else                               { v.spKind = SP_NONE;    v.spRad = NAN;        v.spMs = 0; }
    return v;
}

} // namespace ApDecode
