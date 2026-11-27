#pragma once
// ============================================================================
// ApDecode - the pilot computer's mode, read off the bus.
//
// READ ONLY. Nothing in here - or in ApPipeline, which feeds it - sends a
// single frame: no command, no request, no acknowledgement. The display
// transmits exactly what it transmitted before this module existed.
//
// WHAT IS READ
// Three proprietary single-frame PGNs of manufacturer code 1857, industry
// group 4 (Marine). Their payload starts with the 2-byte proprietary header,
// little-endian: manufacturer code in bits 0..10, two reserved bits (1), the
// industry group in bits 13..15 - "41 9F" on the wire. The header must be
// exactly that: any other two bytes, the same code with the reserved bits
// cleared included, and the frame is ignored.
//
//   PGN 65305, 8 bytes. Byte 2 = model, byte 3 = report type.
//     Report 10, MODE: W = uint32 LE from bytes 4..7. Five bits are known,
//     as the canboat project documents them: 0x08 Standby, 0x10 Auto
//     (heading hold), 0x40 Nav, 0x100 NoDrift, 0x400 Wind. Rules, in this
//     order -
//       0x08 set                 -> Standby, whatever else is set
//       exactly one of the other
//       four, and nothing else   -> that mode
//       0                        -> says nothing (the frame is dropped)
//       anything else            -> engaged, mode not known ("active"):
//                                   two of the four at once, or any bit
//                                   the rules do not know. Never a guessed
//                                   mode - AUTO included.
//     Report 2, STATUS: byte 4 - 2 = manual, 16 = automatic.
//   PGN 65340. Measured on a real installation at the pilot computer,
//     payload 41 9F b2 b3 FE b5 00 80:
//       byte 5: 0xF8 = standby, 0xFA = engaged (anything else says nothing);
//       byte 3 while engaged: 1 = heading, 3 = wind, 4 = nav, 0 = engaged
//       without a sub-mode. Public PGN tables map byte 3 differently - the
//       measurement wins. Other byte-3 values while engaged are taken as
//       "engaged, mode not known" as well (byte 5 is unambiguous).
//   PGN 65341. Byte 4 = mode (2 heading, 3 wind, 10 nav, 11 NoDrift - the
//     values the canboat project documents), bytes 6..7 = setpoint, uint16
//     LE in 0.0001 rad - the held heading or the held wind angle.
//     0xFFFD..0xFFFF = not available. Used for the SETPOINT only; it never
//     decides the mode. The held heading (mode byte 2, 10, 11) and the held
//     wind angle (3) are kept apart; any other mode byte leaves both alone
//     (as a mode it reads "engaged, mode not known", information only), and
//     "not available" clears only the kind its mode byte names.
//
// PGN 127237 (heading/track control) is deliberately not part of this:
// pilots have been seen reporting steering mode 7 there even while engaged,
// so it must never decide the mode. N2kHandler keeps taking heading-to-steer
// and the commanded rudder from it, as before.
//
// WHO COUNTS AS THE PILOT COMPUTER
// Only a source whose ISO NAME (PGN 60928, the address claim) says device
// class 40 (steering and control surfaces), device function 150 and
// manufacturer code 1857. The NAME is learnt passively from the claims on the
// bus (NameTable); ApPipeline also remembers the pilot computer across a
// restart. A frame from a source whose NAME is not known is ignored - see the
// limitation noted at NameTable.
//
// PRIORITY, DEBOUNCE, STALENESS (Tracker)
//   * 65305 report 10 first, then 65340, otherwise "unknown".
//   * A mode changes only after two identical decoded modes in a row from
//     the same source address (the first ever mode, too). Once a mode stands,
//     the input belongs to that address: a second pilot computer is not
//     heard on it until the first has been silent for STALE_MS (the source
//     picker pins one when there are two). Status and setpoints are taken
//     from the address whose mode is shown, too.
//   * A source that has said nothing usable for more than STALE_MS counts as
//     gone; with both gone the mode is "unknown" and the screen shows "--"
//     instead of a STANDBY nobody reported. The setpoints and the status
//     report go stale the same way.
//   * The setpoint shown is the one that belongs to the mode
//     (setpointKindFor): the held heading in AUTO, NAV and NO DRIFT, the held
//     wind angle in WIND, none in STANDBY, and the newer of the two while
//     the mode itself is not known (ACTIVE, or no mode at all).
//
// NOT YET SEEN ON A REAL FRAME: what is said above about 65305 and 65341
// follows canboat's public description only; 65340 and 127237 are the
// measured part. Two conventions the description leaves open are one
// constant each:
//   WIND_SETPOINT_SIGNED    how a wind setpoint is encoded (0..2 pi or int16)
//   WIND_SETPOINT_STBD_LOW  which half of the circle is starboard
//
// Free of Arduino, LVGL, FreeRTOS and the NMEA2000 library on purpose: it
// compiles unchanged on a PC, which is where its tests run. Keep it C++11 -
// the 4-inch board's core still compiles with -std=gnu++11.
// ============================================================================
#include <math.h>
#include <stdint.h>

namespace ApDecode {

// ---- Protocol constants -----------------------------------------------------
static const uint16_t MFR_CODE        = 1857;   // proprietary PGN, manufacturer code 1857
static const uint8_t  INDUSTRY_MARINE = 4;
static const uint8_t  PILOT_CLASS     = 40;     // NAME device class: steering
static const uint8_t  PILOT_FUNCTION  = 150;    // NAME device function: autopilot

// The proprietary header as it is on the wire: the manufacturer code's low
// byte, then its top 3 bits, both reserved bits set and the industry group.
static const uint8_t  PROP_HEADER_B0  = (uint8_t)(MFR_CODE & 0xFF);                    // 0x41
static const uint8_t  PROP_HEADER_B1  =
    (uint8_t)(((MFR_CODE >> 8) & 0x07) | 0x18 | (INDUSTRY_MARINE << 5));               // 0x9F

static const uint32_t PGN_ADDRESS_CLAIM = 60928;
static const uint32_t PGN_MODE_REPORT   = 65305;
static const uint32_t PGN_STATE         = 65340;
static const uint32_t PGN_SETPOINT      = 65341;

static const uint8_t  REPORT_STATUS = 2;        // 65305 byte 3
static const uint8_t  REPORT_MODE   = 10;

// A mode, status or setpoint older than this is gone ("more than 10 s").
static const uint32_t STALE_MS = 10000;

// ---- Conventions still to be confirmed on a real frame ----------------------
// Wind setpoint (65341, mode byte 3). Two separate questions, one constant
// each; the decoder applies them in exactly one place (decode(), PGN_SETPOINT).
//
// 1. Encoding. false: unsigned 0..2 pi like PGN 130306's wind angle (a raw
//    value above 62832 cannot occur then and is treated as "not available";
//    ApPipeline logs it once). true: signed int16, -pi..+pi. That log is a
//    hint, not a test: a signed encoding lands above 62832 only for port
//    angles under 15.5 degrees, so a quiet log proves nothing.
static const bool WIND_SETPOINT_SIGNED = false;
// 2. Side. true: the half the value counts up into first - 0..pi unsigned,
//    positive signed - is starboard, the way 130306 counts clockwise from
//    the bow. The result is handed on as -pi..+pi with port negative (the
//    DataModel's convention).
static const bool WIND_SETPOINT_STBD_LOW = true;
//
// HOW TO SETTLE BOTH ON BOARD: record one 65341 frame with the wind set on
// port and one on starboard, x degrees each, and read raw = bytes 6..7 LE.
// With r = x * 174.53 (x degrees in 0.0001 rad):
//   port raw ~ 62832 - r, starboard raw ~ r          -> false, true (as now)
//   port raw ~ 65536 - r, starboard raw ~ r          -> true,  true
//   port raw ~ r,         starboard raw ~ 62832 - r  -> false, false
//   port raw ~ r,         starboard raw ~ 65536 - r  -> true,  false
// The two encodings differ by 2704 (15.5 degrees) only, so compare against
// both numbers - a single port frame below pi tells the side, not the
// encoding.

// ---- What the pilot computer is doing ---------------------------------------
// Kept in the DataModel as uint8_t. Explicit numbers that stay as they are:
// a mode added later takes a free one (5, 7..14). None of them is stored or
// sent as a number - /api/data and the log use modeKey().
enum Mode : uint8_t {
    MODE_UNKNOWN  = 0,    // no fresh status - the screen shows "--"
    MODE_STANDBY  = 1,
    MODE_AUTO     = 2,    // heading hold
    MODE_WIND     = 3,
    MODE_NAV      = 4,
    MODE_NO_DRIFT = 6,
    MODE_ACTIVE   = 15,   // engaged, the mode itself not known
    MODE_COUNT
};
enum Status : uint8_t { STATUS_UNKNOWN = 0, STATUS_MANUAL, STATUS_AUTOMATIC };
enum SetpointKind : uint8_t { SP_NONE = 0, SP_HEADING, SP_WIND };
// Which input decided the mode (DataModel::apModeSource).
enum Source : uint8_t { SRC_NONE = 0, SRC_MODE_REPORT, SRC_STATE, SRC_DEMO };
// Why a frame said nothing (Facts::ignored). For tests and the log only.
enum Ignore : uint8_t {
    IGN_NONE = 0,
    IGN_PGN,              // not one of the three PGNs
    IGN_SHORT,            // too few bytes
    IGN_HEADER,           // not "41 9F": another manufacturer, group or reserved bits
    IGN_NAME_UNKNOWN,     // no address claim seen for this source yet
    IGN_NOT_PILOT,        // NAME known, but not the pilot computer
    IGN_REPORT,           // 65305 report type this module does not read
    IGN_UNDECODED         // understood frame, but no rule matched the value
};

inline bool isEngaged(Mode m) { return m != MODE_UNKNOWN && m != MODE_STANDBY; }
// The setpoint that belongs to a mode: SP_HEADING in AUTO, NAV and NO DRIFT,
// SP_WIND in WIND, SP_NONE in every other mode - STANDBY holds no course, and
// for ACTIVE/UNKNOWN the Tracker shows the newer of the two instead.
SetpointKind setpointKindFor(Mode m);
// Short ASCII key for logs and /api/data ("standby", "no_drift", ...).
const char *modeKey(Mode m);

// ---- ISO NAME (PGN 60928) ---------------------------------------------------
// The 8 data bytes of an address claim are the 64-bit NAME, little-endian:
//   bits  0..20  unique number          bits 40..47  device function
//   bits 21..31  manufacturer code      bit  48      reserved
//   bits 32..34  device instance lower  bits 49..55  device class
//   bits 35..39  device instance upper  bits 56..59  system instance
//                                       bits 60..62  industry group
//                                       bit  63      arbitrary address capable
// So: manufacturer = byte 2 >> 5 | byte 3 << 3, function = byte 5,
// class = byte 6 >> 1.
inline uint16_t nameManufacturer(uint64_t n) { return (uint16_t)((n >> 21) & 0x7FF); }
inline uint8_t  nameFunction(uint64_t n)     { return (uint8_t)((n >> 40) & 0xFF); }
inline uint8_t  nameClass(uint64_t n)        { return (uint8_t)((n >> 49) & 0x7F); }
inline uint8_t  nameIndustry(uint64_t n)     { return (uint8_t)((n >> 60) & 0x07); }
// false for fewer than 8 bytes.
bool nameFromClaim(const uint8_t *d, uint8_t len, uint64_t &name);
bool isPilotComputer(uint64_t name);

// Address -> NAME, learnt from the address claims seen on the bus.
//
// Fixed table, no heap. Every claim is recorded; when the table is full, a
// claim of a NAME that is not a pilot computer is not stored (frames from
// such a source are dropped either way), and a pilot computer's claim takes
// the slot of an entry that is not one.
//
// LIMITATION: nothing here asks the bus for claims (that would be a
// transmission). A pilot computer that claimed its address before this
// display started listening stays unknown - and the mode "--" - until it
// claims again: when it is switched on, when it has to change its address,
// or when any device on the bus sends an ISO request for PGN 60928 (chart
// plotters typically do when they build their device list). Claims sent
// while demo mode runs are missed the same way: the bus is not parsed then.
// ApPipeline narrows this down by remembering the pilot computer (NAME and
// address) across a restart and entering it here at boot; that covers the
// common case, a display restarted on its own while the pilot keeps its
// address.
class NameTable {
public:
    enum { CAPACITY = 32 };

    NameTable() { reset(); }
    void reset();

    // One address claim as received. Source address 254 is a "cannot claim":
    // that NAME no longer has an address. A NAME seen at a new address is
    // removed from its old one; a claim for an address that holds another
    // NAME replaces it (that device lost the address and will claim another
    // or send "cannot claim"). Returns false if the frame was unusable.
    bool claim(uint8_t sa, const uint8_t *d, uint8_t len);
    void claimName(uint8_t sa, uint64_t name);

    bool lookup(uint8_t sa, uint64_t &name) const;
    int  count() const;

private:
    struct Entry { uint64_t name; uint8_t sa; bool used; };
    Entry e[CAPACITY];
};

// ---- One decoded frame ------------------------------------------------------
struct Facts {
    Ignore   ignored;        // != IGN_NONE: the frame says nothing usable
    uint32_t pgn;
    uint8_t  sa;
    uint8_t  model;          // 65305 byte 2 (0xFF otherwise)
    uint8_t  report;         // 65305 byte 3 (0xFF otherwise)
    uint32_t word;           // 65305 report 10: W
    // A status for the mode (65305 report 10, 65340).
    bool     hasMode;
    Mode     mode;
    bool     engaged;
    // 65305 report 2.
    bool     hasStatus;
    Status   status;
    // 65341. hasSetpoint = a 65341 frame was decoded; setpointKind says which
    // setpoint it carries (SP_NONE: neither - the Tracker leaves both alone).
    // setpointMode is its mode byte as a Mode (MODE_ACTIVE for a byte that is
    // not one of the four known values) - information only.
    bool         hasSetpoint;
    Mode         setpointMode;
    SetpointKind setpointKind;
    float        setpointRad;   // heading 0..2 pi; wind -pi..+pi, port negative; NaN = n/a
    uint16_t     setpointRaw;   // bytes 6..7 as received
    // A wind setpoint above 2 pi while WIND_SETPOINT_SIGNED is false: only a
    // signed encoding produces it. setpointRad is NaN then (the wind setpoint
    // is cleared rather than shown wrong); ApPipeline logs it.
    bool         setpointOutOfRange;

    Facts();
};

// Decode one frame. nameKnown/name: what the NameTable says about `sa`.
Facts decode(uint32_t pgn, uint8_t sa, const uint8_t *d, uint8_t len,
             bool nameKnown, uint64_t name);

// Rules of 65305 report 10, separately for the tests. MODE_UNKNOWN: the word
// says nothing (it is 0).
Mode modeFromWord(uint32_t w);

// ---- Priority, debounce, staleness ------------------------------------------
class Tracker {
public:
    struct View {
        Mode         mode;
        bool         engaged;
        Source       source;      // which input decided the mode
        uint8_t      sa;          // its address (0xFF = none)
        uint32_t     modeMs;      // when that input last said something usable
        Status       status;      // 65305 report 2, fresh only
        // The two setpoints of 65341, fresh only (NaN and 0 otherwise).
        float        hdgSpRad;    // 0..2 pi
        uint32_t     hdgSpMs;
        float        windSpRad;   // -pi..+pi, port negative
        uint32_t     windSpMs;
        // The one that belongs to the mode (setpointKindFor); SP_NONE, NaN
        // and 0 when there is none.
        SetpointKind spKind;
        float        spRad;
        uint32_t     spMs;
    };

    Tracker() { reset(); }
    void reset();
    // A decoded frame (ignored ones are dropped here).
    void feed(const Facts &f, uint32_t nowMs);
    // Demo mode: sets everything at once, no debounce; it outranks the bus
    // and goes stale like any other input when the demo stops feeding it.
    void demo(Mode m, Status s, SetpointKind k, float spRad, uint32_t nowMs);
    // Forget inputs that have been quiet for more than STALE_MS, so one that
    // comes back has to pass the debounce again.
    void expire(uint32_t nowMs);
    View view(uint32_t nowMs) const;

private:
    struct Chan {
        bool     have;            // heard within STALE_MS (see expire)
        Mode     stable;          // debounced mode, MODE_UNKNOWN until two agree
        uint8_t  sa;              // address the stable mode came from (0xFF = none)
        Mode     pend;            // last decoded mode, its source and how often in a row
        uint8_t  pendSa;
        uint8_t  pendCount;
        uint32_t lastMs;
    };
    struct Sp { bool have; float rad; uint32_t ms; };
    enum { CH_MODE_REPORT = 0, CH_STATE, CH_DEMO, CH_N };
    Chan     ch[CH_N];
    Status   status;   uint32_t statusMs; bool haveStatus;
    Sp       spHdg, spWind;

    static bool fresh(bool have, uint32_t lastMs, uint32_t nowMs);
    void feedMode(Chan &c, Mode m, uint8_t sa, uint32_t nowMs);
    // Index into ch[] of the input that decides the mode now, -1 for none.
    int  deciding(uint32_t nowMs) const;
};

} // namespace ApDecode
