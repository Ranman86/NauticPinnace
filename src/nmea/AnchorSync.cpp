// ============================================================================
// AnchorSync.cpp - see AnchorSync.h for the wire format.
//
// The codec deliberately depends on nothing but stdint: no NMEA2000, no LVGL,
// no DataModel. That is what lets anchorSyncSelfTest() run on the simulator,
// on the device and in any future host test without dragging the bus in.
// ============================================================================
#include "AnchorSync.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>

// ---- little-endian writers/readers ------------------------------------------
// Written out by hand rather than memcpy'd from a struct: the wire order must
// not depend on the compiler's idea of endianness or padding.
static inline void putU16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8);
}
static inline void putU32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v       & 0xFF); p[1] = (uint8_t)((v >>  8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static inline uint16_t getU16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t getU32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static constexpr int32_t  COORD_NA  = 0x7FFFFFFF;
static constexpr uint16_t RADIUS_NA = 0xFFFF;
static constexpr uint32_t UTC_NA    = 0xFFFFFFFFUL;
// (4 << 13) | (3 << 11) | 2046 - industry group 4, reserved bits 3, mfg 2046.
static constexpr uint16_t MFG_HEADER =
    (uint16_t)((4u << 13) | (3u << 11) | ANCHOR_SYNC_MFG);

// Degrees -> 1e-7 units. lround, not a truncating cast: 54.3216789 in double
// is 54.32167889999999..., and truncating turns the last digit into a 8.
static inline int32_t degToUnits(double deg) {
    return (int32_t)llround(deg * 1e7);
}

size_t anchorSyncEncode(const AnchorSyncState &s, uint8_t *out, size_t outSz) {
    if (!out || outSz < ANCHOR_SYNC_LEN) return 0;
    memset(out, 0, ANCHOR_SYNC_LEN);

    putU16(out + 0, MFG_HEADER);
    out[2] = ANCHOR_SYNC_MSG_ID;
    out[3] = ANCHOR_SYNC_VERSION;
    out[4] = (uint8_t)((s.armed ? 0x01 : 0x00) | (s.posValid ? 0x02 : 0x00));

    const bool havePos = s.posValid && !isnan(s.lat) && !isnan(s.lon);
    putU32(out +  5, (uint32_t)(havePos ? degToUnits(s.lat) : COORD_NA));
    putU32(out +  9, (uint32_t)(havePos ? degToUnits(s.lon) : COORD_NA));

    // Tenths of a metre, not metres. 0xFFFF is reserved for "not available",
    // so a radius that would land on it is pulled one tenth short.
    uint16_t r = RADIUS_NA;
    if (s.radiusM > 0.f && !isnan(s.radiusM)) {
        long t = lround((double)s.radiusM * 10.0);
        if (t < 0) t = 0;
        if (t >= (long)RADIUS_NA) t = (long)RADIUS_NA - 1;
        r = (uint16_t)t;
    }
    putU16(out + 13, r);

    putU32(out + 15, s.revUtc);
    out[19] = s.revNode;
    putU32(out + 20, s.setUtc ? s.setUtc : UTC_NA);
    return ANCHOR_SYNC_LEN;
}

bool anchorSyncDecode(const uint8_t *in, size_t len, AnchorSyncState &out) {
    if (!in || len < ANCHOR_SYNC_LEN)            return false;
    if (getU16(in + 0) != MFG_HEADER)            return false;   // not ours
    if (in[2] != ANCHOR_SYNC_MSG_ID)             return false;
    if (in[3] != ANCHOR_SYNC_VERSION)            return false;

    AnchorSyncState s;
    s.armed    = (in[4] & 0x01) != 0;
    s.posValid = (in[4] & 0x02) != 0;

    // The range check applies only where the sender claims a position. A cleared
    // flag means the coordinate fields carry nothing, and rejecting the whole
    // message over what is in them would throw away a perfectly good "anchor is
    // up" from a peer that fills them with something other than the sentinel.
    const int32_t latU = (int32_t)getU32(in + 5);
    const int32_t lonU = (int32_t)getU32(in + 9);
    s.lat = s.lon = 0.0;
    if (s.posValid) {
        if (latU == COORD_NA || lonU == COORD_NA) {
            s.posValid = false;                  // n/a beats the flag
        } else {
            s.lat = (double)latU * 1e-7;
            s.lon = (double)lonU * 1e-7;
            // Out of range is a corrupt or foreign message, not a position to
            // adopt - the whole message goes, rather than half of it.
            if (s.lat < -90.0  || s.lat > 90.0)  return false;
            if (s.lon < -180.0 || s.lon > 180.0) return false;
        }
    }

    const uint16_t r = getU16(in + 13);
    s.radiusM = (r == RADIUS_NA) ? 0.f : (float)r * 0.1f;

    s.revUtc  = getU32(in + 15);
    s.revNode = in[19];
    const uint32_t su = getU32(in + 20);
    s.setUtc  = (su == UTC_NA) ? 0 : su;

    out = s;
    return true;
}

// ---- self test ---------------------------------------------------------------
// The two vectors below were captured from the running NauticPi
// (`NauticPi --test-anchor-sync`). They are the contract: if this stops
// passing, the two devices no longer understand each other, and that is worth
// failing loudly at boot rather than discovering it at anchor.
namespace {

struct Vector {
    const char      *name;
    AnchorSyncState  state;
    uint8_t          bytes[ANCHOR_SYNC_LEN];
};

void dumpHex(const char *tag, const uint8_t *b, size_t n) {
    Serial.printf("[anchorsync]   %s", tag);
    for (size_t i = 0; i < n; i++) Serial.printf(" %02X", b[i]);
    Serial.println();
}

}  // namespace

bool anchorSyncSelfTest() {
    Vector vecs[2] = {};

    // Armed, 54.3216789 N / 10.1234567 E, radius 42.5 m,
    // revUtc 1790618645, node 1 (NauticPi), setUtc 1790186645.
    vecs[0].name              = "armed, node 1";
    vecs[0].state.armed       = true;
    vecs[0].state.posValid    = true;
    vecs[0].state.lat         = 54.3216789;
    vecs[0].state.lon         = 10.1234567;
    vecs[0].state.radiusM     = 42.5f;
    vecs[0].state.revUtc      = 1790618645UL;
    vecs[0].state.revNode     = ANCHOR_NODE_NAUTICPI;
    vecs[0].state.setUtc      = 1790186645UL;
    {
        const uint8_t b[ANCHOR_SYNC_LEN] = {
            0xFE, 0x9F, 0x01, 0x01, 0x03, 0x95, 0xD4, 0x60,
            0x20, 0x87, 0xB7, 0x08, 0x06, 0xA9, 0x01, 0x15,
            0xAC, 0xBA, 0x6A, 0x01, 0x95, 0x14, 0xB4, 0x6A };
        memcpy(vecs[0].bytes, b, ANCHOR_SYNC_LEN);
    }

    // No anchor set, radius 30 m, revUtc 1790618700, node 2 (this firmware).
    vecs[1].name              = "no anchor, node 2";
    vecs[1].state.armed       = false;
    vecs[1].state.posValid    = false;
    vecs[1].state.radiusM     = 30.0f;
    vecs[1].state.revUtc      = 1790618700UL;
    vecs[1].state.revNode     = ANCHOR_NODE_PINNACE;
    vecs[1].state.setUtc      = 0;
    {
        const uint8_t b[ANCHOR_SYNC_LEN] = {
            0xFE, 0x9F, 0x01, 0x01, 0x00, 0xFF, 0xFF, 0xFF,
            0x7F, 0xFF, 0xFF, 0xFF, 0x7F, 0x2C, 0x01, 0x4C,
            0xAC, 0xBA, 0x6A, 0x02, 0xFF, 0xFF, 0xFF, 0xFF };
        memcpy(vecs[1].bytes, b, ANCHOR_SYNC_LEN);
    }

    bool ok = true;

    // The PGN itself. It has moved once (130900 was taken twice over) and the
    // number lives in exactly one place; a silent third move would look like a
    // dead bus on one side and perfect health on the other.
    if (ANCHOR_SYNC_PGN != 131035UL) {
        ok = false;
        Serial.printf("[anchorsync] FAIL: PGN is %lu, expected 131035\n",
                      (unsigned long)ANCHOR_SYNC_PGN);
    }

    for (int i = 0; i < 2; i++) {
        const Vector &v = vecs[i];

        // Encoder: byte for byte.
        uint8_t enc[ANCHOR_SYNC_LEN];
        const size_t n = anchorSyncEncode(v.state, enc, sizeof(enc));
        if (n != ANCHOR_SYNC_LEN || memcmp(enc, v.bytes, ANCHOR_SYNC_LEN) != 0) {
            ok = false;
            Serial.printf("[anchorsync] FAIL encode: %s\n", v.name);
            dumpHex("want:", v.bytes, ANCHOR_SYNC_LEN);
            dumpHex("got: ", enc,     ANCHOR_SYNC_LEN);
        }

        // Decoder: back into the same state.
        AnchorSyncState d;
        if (!anchorSyncDecode(v.bytes, ANCHOR_SYNC_LEN, d)) {
            ok = false;
            Serial.printf("[anchorsync] FAIL decode (rejected): %s\n", v.name);
        } else {
            const bool same =
                d.armed    == v.state.armed    &&
                d.posValid == v.state.posValid &&
                d.revUtc   == v.state.revUtc   &&
                d.revNode  == v.state.revNode  &&
                d.setUtc   == v.state.setUtc   &&
                fabsf(d.radiusM - v.state.radiusM) < 0.05f &&
                (!v.state.posValid ||
                 (fabs(d.lat - v.state.lat) < 1e-7 &&
                  fabs(d.lon - v.state.lon) < 1e-7));
            if (!same) {
                ok = false;
                Serial.printf("[anchorsync] FAIL decode (mismatch): %s\n", v.name);
                Serial.printf("[anchorsync]   got armed=%d pos=%d %.7f/%.7f r=%.1f "
                              "rev=%lu/%u set=%lu\n",
                              d.armed, d.posValid, d.lat, d.lon, d.radiusM,
                              (unsigned long)d.revUtc, d.revNode,
                              (unsigned long)d.setUtc);
            }
        }
    }

    // Rejections. Each of these has been mistaken for "the protocol is wrong"
    // at least once somewhere, so they are checked rather than assumed.
    {
        uint8_t b[ANCHOR_SYNC_LEN];
        memcpy(b, vecs[0].bytes, ANCHOR_SYNC_LEN);
        AnchorSyncState d;

        if (anchorSyncDecode(b, ANCHOR_SYNC_LEN - 1, d)) {
            ok = false; Serial.println("[anchorsync] FAIL: short message accepted");
        }
        b[0] = 0xA3; b[1] = 0x99;                        // a stereo maker, mfg 419
        if (anchorSyncDecode(b, ANCHOR_SYNC_LEN, d)) {
            ok = false; Serial.println("[anchorsync] FAIL: foreign manufacturer accepted");
        }
        memcpy(b, vecs[0].bytes, ANCHOR_SYNC_LEN);
        b[2] = 0x02;                                     // other message id
        if (anchorSyncDecode(b, ANCHOR_SYNC_LEN, d)) {
            ok = false; Serial.println("[anchorsync] FAIL: foreign message id accepted");
        }
        memcpy(b, vecs[0].bytes, ANCHOR_SYNC_LEN);
        b[3] = 0x02;                                     // other format version
        if (anchorSyncDecode(b, ANCHOR_SYNC_LEN, d)) {
            ok = false; Serial.println("[anchorsync] FAIL: foreign format version accepted");
        }
        memcpy(b, vecs[0].bytes, ANCHOR_SYNC_LEN);
        putU32(b + 5, (uint32_t)degToUnits(91.0));       // latitude out of range
        if (anchorSyncDecode(b, ANCHOR_SYNC_LEN, d)) {
            ok = false; Serial.println("[anchorsync] FAIL: latitude 91 accepted");
        }
        memcpy(b, vecs[0].bytes, ANCHOR_SYNC_LEN);
        putU32(b + 9, (uint32_t)degToUnits(-181.0));     // longitude out of range
        if (anchorSyncDecode(b, ANCHOR_SYNC_LEN, d)) {
            ok = false; Serial.println("[anchorsync] FAIL: longitude -181 accepted");
        }

        // ...but ONLY where a position is claimed. With bit 1 clear the
        // coordinate fields carry nothing, and a peer that leaves something
        // other than the sentinel in them must still be understood.
        memcpy(b, vecs[0].bytes, ANCHOR_SYNC_LEN);
        b[4] = 0x01;                                     // armed, position NOT valid
        putU32(b + 5, (uint32_t)degToUnits(91.0));
        putU32(b + 9, (uint32_t)degToUnits(-181.0));
        if (!anchorSyncDecode(b, ANCHOR_SYNC_LEN, d)) {
            ok = false;
            Serial.println("[anchorsync] FAIL: out-of-range coords rejected although "
                           "the position flag was clear");
        } else if (d.posValid) {
            ok = false;
            Serial.println("[anchorsync] FAIL: position reported valid with the flag clear");
        }
    }

    // Arbitration, including the two rules that are easy to get wrong.
    {
        AnchorSyncState a, b;
        a.revUtc = 100; a.revNode = 2;

        b.revUtc = 101; b.revNode = 2;
        if (!anchorSyncShouldAdopt(b, a)) { ok = false; Serial.println("[anchorsync] FAIL: newer not adopted"); }
        b.revUtc =  99;
        if (anchorSyncShouldAdopt(b, a))  { ok = false; Serial.println("[anchorsync] FAIL: older adopted"); }
        b.revUtc = 100; b.revNode = 1;                   // same second, lower node
        if (!anchorSyncShouldAdopt(b, a)) { ok = false; Serial.println("[anchorsync] FAIL: tie not won by lower node"); }
        b.revNode = 3;                                   // same second, higher node
        if (anchorSyncShouldAdopt(b, a))  { ok = false; Serial.println("[anchorsync] FAIL: tie won by higher node"); }
        b.revUtc = 100; b.revNode = 2;                   // identical - a heartbeat
        if (anchorSyncShouldAdopt(b, a))  { ok = false; Serial.println("[anchorsync] FAIL: heartbeat adopted"); }
    }

    // Revision stamping: a lagging clock must still move the revision forward.
    {
        if (anchorSyncStampRevision(1790618700UL, 100) != 1790618700UL) {
            ok = false; Serial.println("[anchorsync] FAIL: good clock not used");
        }
        if (anchorSyncStampRevision(100, 1790618700UL) != 1790618701UL) {
            ok = false; Serial.println("[anchorsync] FAIL: lagging clock not bumped");
        }
        if (anchorSyncStampRevision(0, 0) != 1) {
            ok = false; Serial.println("[anchorsync] FAIL: no clock at all not bumped");
        }
    }

    // The cases an adversarial review found in the first version of this node.
    // They are checked here rather than remembered, because every one of them
    // looked right until it was written out as a sequence.
    {
        // A state the PEER holds but that WE stamped (rule 1: the adopter keeps
        // revNode unchanged). The first version dropped every revNode == 2
        // message as "our own echo", which threw away the only messages that
        // could bring this device back into agreement after it lost its config.
        AnchorSyncState own;  own.revUtc = 0;    own.revNode = ANCHOR_NODE_PINNACE;
        AnchorSyncState peer; peer.revUtc = 1790618645UL; peer.revNode = ANCHOR_NODE_PINNACE;
        if (!anchorSyncShouldAdopt(peer, own)) {
            ok = false;
            Serial.println("[anchorsync] FAIL: node-2 state held by the peer not adopted "
                           "after a config loss");
        }

        // A genuine echo of our own current broadcast must still be ignored -
        // that is what makes the revNode filter unnecessary rather than merely
        // harmful.
        own.revUtc = 1790618645UL;
        if (anchorSyncShouldAdopt(peer, own)) {
            ok = false; Serial.println("[anchorsync] FAIL: own echo adopted");
        }

        // A local change made after a message was decoded but before it was
        // applied must win. This is the comparison applyPending() repeats.
        AnchorSyncState parked; parked.revUtc = 1005; parked.revNode = ANCHOR_NODE_NAUTICPI;
        AnchorSyncState afterLocalTap;
        afterLocalTap.revUtc  = anchorSyncStampRevision(1006, 1000);
        afterLocalTap.revNode = ANCHOR_NODE_PINNACE;
        if (anchorSyncShouldAdopt(parked, afterLocalTap)) {
            ok = false;
            Serial.println("[anchorsync] FAIL: parked state would overwrite a newer "
                           "local change");
        }

        // The claim: an anchor held with no revision must end up ABOVE whatever
        // the peer is carrying, or an update switches off a live drag alarm.
        const uint32_t peerRev  = 1790618645UL;
        const uint32_t claimNoClock = anchorSyncStampRevision(0, peerRev);
        if (claimNoClock <= peerRev) {
            ok = false; Serial.println("[anchorsync] FAIL: claim does not beat the peer");
        }
    }

    Serial.printf("[anchorsync] self test %s\n", ok ? "PASS" : "*** FAILED ***");
    return ok;
}
