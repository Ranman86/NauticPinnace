#pragma once
// ============================================================================
// AnchorSync - anchor-watch state shared with NauticPi over NMEA 2000.
//
// Whoever sets, moves, arms or clears the anchor alarm on one device has it
// the same way on the other about a second later, in both directions. Both
// sides decide who wins from the message alone, without negotiating.
//
// WIRE FORMAT - PGN 131035, proprietary, broadcast (0xFF), priority 6, fast
// packet, manufacturer 2046, industry group 4 (marine). 24 payload bytes,
// little endian throughout:
//
//   0  2  manufacturer header  FE 9F  = (4 << 13) | (3 << 11) | 2046
//   2  1  message id           0x01 = anchor state
//   3  1  format version       0x01
//   4  1  flags                bit 0 armed, bit 1 position valid, 2-7 reserved
//   5  4  latitude             int32, 1e-7 deg, north positive, 0x7FFFFFFF n/a
//   9  4  longitude            int32, 1e-7 deg, east positive,  0x7FFFFFFF n/a
//  13  2  swing radius         uint16, 0.1 m, 0xFFFF n/a
//  15  4  revUtc               uint32, Unix seconds UTC - when last CHANGED
//  19  1  revNode              uint8 - who changed it: 1 NauticPi, 2 this
//  20  4  setUtc               uint32, Unix seconds UTC - when the anchor fell,
//                              0xFFFFFFFF = none
//
// revUtc AND setUtc ARE TWO DIFFERENT THINGS and must never be merged. setUtc
// stands still at the moment the anchor fell and is what the user is shown.
// revUtc moves on EVERY change, arming and radius included - order by setUtc
// and every disarm is silently swallowed while a new anchor still travels.
//
// A later version may APPEND fields. It may not reinterpret the ones above.
//
// THE HEADER IS NOT A SIGNATURE. 2046 is the ttlappalainen library's default
// for open-source devices and sits in hundreds of DIY projects - the Nautinect
// autopilot sends its own messages under it on 130970-130972 - so FE 9F only
// says "some DIY device". It does keep every commercial sender out (a stereo
// maker's 419 = A3 99, B&G 381), and telling our message from another hobbyist's rests on
// the PGN nobody else uses, then on the message id, the format version, the
// exact length and the range checks. All of them are load-bearing; none of them
// may be dropped as "surely redundant".
// ============================================================================
#include <stdint.h>
#include <stddef.h>

// Node numbers. Lower wins a tie on revUtc, so these must match on both sides.
enum : uint8_t {
    ANCHOR_NODE_NAUTICPI   = 1,
    ANCHOR_NODE_PINNACE    = 2,   // this firmware
};

// THE ONE PLACE THE PGN IS WRITTEN DOWN. It has moved once already: 130900 was
// picked from memory and turned out to be taken twice over - Xantrex AC status
// in canboat, Santa Cruz Instruments in their own firmware. 131035 (0x1FFDB)
// was then chosen by measurement, as the centre of the widest gap left after
// canboat 8.2.1 and its history, ~130 other decoders and real bus captures were
// swept. Nothing else in this tree may spell the number out; see the full
// reasoning in SailTrimMonitor/ANCHOR-SYNC.md.
//
// It stays inside 130816..131071, so the wire format is untouched by the move:
// the PGN travels in the CAN identifier, not in the payload, and the reference
// byte sequences below are as valid as they were.
static constexpr uint32_t ANCHOR_SYNC_PGN      = 131035UL;
static constexpr size_t   ANCHOR_SYNC_LEN      = 24;
static constexpr uint16_t ANCHOR_SYNC_MFG      = 2046;
static constexpr uint8_t  ANCHOR_SYNC_MSG_ID   = 0x01;
static constexpr uint8_t  ANCHOR_SYNC_VERSION  = 0x01;
// Broadcast every 10 s on top of every local change, so a device that was just
// switched on agrees within ten seconds.
static constexpr uint32_t ANCHOR_SYNC_PERIOD_MS = 10000;

struct AnchorSyncState {
    bool     armed    = false;    // drag alarm armed
    bool     posValid = false;    // an anchor position is held
    double   lat      = 0.0;      // degrees, north positive; only if posValid
    double   lon      = 0.0;      // degrees, east positive;  only if posValid
    float    radiusM  = 0.f;      // swing radius in metres; 0 = not available
    uint32_t revUtc   = 0;        // when this state was last CHANGED
    uint8_t  revNode  = 0;        // who changed it
    uint32_t setUtc   = 0;        // when the anchor fell; 0 = none
};

// Writes exactly ANCHOR_SYNC_LEN bytes. Returns the length written.
size_t anchorSyncEncode(const AnchorSyncState &s, uint8_t *out, size_t outSz);

// Returns false - and leaves `out` untouched - for anything that is not one of
// ours: short, wrong manufacturer, wrong message id, wrong format version, or
// a position outside +-90 / +-180. Rejecting beats guessing.
bool anchorSyncDecode(const uint8_t *in, size_t len, AnchorSyncState &out);

// Whoever has the newer revision wins; on the same second the lower node wins.
// Both sides run this identically, so they cannot disagree.
inline bool anchorSyncShouldAdopt(const AnchorSyncState &incoming,
                                  const AnchorSyncState &own) {
    if (incoming.revUtc > own.revUtc) return true;
    if (incoming.revUtc == own.revUtc && incoming.revNode < own.revNode) return true;
    return false;
}

// Stamp for a LOCAL change. Not plainly "now": a device whose clock lags - an
// ESP32 with no GNSS fix, a Pi with no NTP - would otherwise stamp its own
// change older than the state it just replaced, the other side would win, and
// the user would watch their own tap being undone. The +1 branch is what
// carries a change through on a device with no clock at all.
inline uint32_t anchorSyncStampRevision(uint32_t nowUtc, uint32_t ownRevUtc) {
    const uint32_t bumped = ownRevUtc + 1;
    return (nowUtc > bumped) ? nowUtc : bumped;
}

// Checks the encoder and the decoder against the reference vectors captured
// from the running NauticPi. Prints its own failures. Call it at boot.
bool anchorSyncSelfTest();
