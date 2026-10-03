// ============================================================================
// AnchorSyncNode.cpp - see AnchorSyncNode.h.
// ============================================================================
#include "AnchorSyncNode.h"
#include "DataModel.h"
#include "../config/Config.h"
#include <Arduino.h>
#include <math.h>
#include <time.h>
#ifndef SIMULATOR
#include <NMEA2000.h>
extern tNMEA2000 &NMEA2000;
#endif

// Zero is a legitimate answer, not an error: rule 2 of the arbitration
// (revUtc = max(now, own + 1)) exists precisely so a device without a clock can
// still carry its own change through. Declared in the header because the anchor
// screen needs it too, for the "when the anchor fell" stamp.
uint32_t anchorSyncNowUtcOrZero() {
    // SNTP, when WiFi has run: the C library clock keeps ticking by itself.
    const time_t t = time(nullptr);
    if (t > 1700000000) return (uint32_t)t;       // later than 2023-11 => set

    // Otherwise the bus clock (PGN 126992 / 129033), advanced by the time since
    // it was last heard - the DataModel holds a sample, not a running clock.
    uint16_t days; double secOfDay; uint32_t lastMs; bool valid;
    {
        auto lk = data.lock();
        days = data.sysDays; secOfDay = data.sysSecOfDay;
        lastMs = data.lastTimeUpdate; valid = data.timeValid;
    }
    if (!valid || days == 0) return 0;
    const uint32_t age = (millis() - lastMs) / 1000u;
    return (uint32_t)((uint32_t)days * 86400UL + (uint32_t)secOfDay + age);
}

namespace AnchorSyncNode {
namespace {

bool     s_canSend     = false;
bool     s_txPending   = false;   // a local change wants to go out now
uint32_t s_lastTxMs    = 0;
// Set at boot when this device holds a live anchor with no revision at all -
// see the note in begin(). Cleared by the first peer message.
bool     s_claimPending = false;
bool     s_peerSeen     = false;   // first valid peer message logged?

// Pending adoption, handed from the bus task (core 0) to the display task
// (core 1). A spinlock rather than the DataModel mutex: the critical section is
// a 40-byte struct copy, and the bus task must not be able to block on the
// display task. The simulator has no second task and no FreeRTOS port layer,
// so the guard compiles away there.
#ifdef SIMULATOR
#define ANCHOR_LOCK()   do {} while (0)
#define ANCHOR_UNLOCK() do {} while (0)
#else
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define ANCHOR_LOCK()   portENTER_CRITICAL(&s_mux)
#define ANCHOR_UNLOCK() portEXIT_CRITICAL(&s_mux)
#endif
bool             s_hasPend = false;
AnchorSyncState  s_pend;

// The anchor state as this device currently holds it.
AnchorSyncState localState() {
    AnchorSyncState s;
    const auto &c  = appConfig.cfg;
    s.armed    = c.anchorAlarmOn;
    s.posValid = c.anchorSet && !isnan(c.anchorLat) && !isnan(c.anchorLon);
    s.lat      = s.posValid ? c.anchorLat : 0.0;
    s.lon      = s.posValid ? c.anchorLon : 0.0;
    s.radiusM  = c.anchorRadius;
    s.revUtc   = c.anchorRevUtc;
    s.revNode  = c.anchorRevNode ? c.anchorRevNode : ANCHOR_NODE_PINNACE;
    s.setUtc   = c.anchorSetUtc;
    return s;
}

void broadcast() {
#ifndef SIMULATOR
    if (!s_canSend) return;
    uint8_t payload[ANCHOR_SYNC_LEN];
    if (anchorSyncEncode(localState(), payload, sizeof(payload)) != ANCHOR_SYNC_LEN)
        return;

    tN2kMsg msg;
    // Init() rather than SetPGN(): it sets priority, destination, length AND
    // clears the multi-packet flag in one go. The source passed here is
    // irrelevant - SendMsg() overwrites it with this node's claimed address.
    msg.Init(6 /* priority */, ANCHOR_SYNC_PGN, 0 /* source */, 0xFF /* broadcast */);
    for (size_t i = 0; i < ANCHOR_SYNC_LEN; i++) msg.AddByte(payload[i]);
    NMEA2000.SendMsg(msg);
    s_lastTxMs = millis();
#endif
}

}  // namespace

void begin(bool canSend) {
    s_canSend = canSend;
    anchorSyncSelfTest();
    // An anchor that is down but carries no revision: a config written before
    // this firmware knew about the sync, or one restored from an export. Left
    // at revUtc 0 it loses to the first peer heartbeat, and a device lying at
    // anchor would have its watch switched off within ten seconds of an update,
    // without anyone touching it.
    //
    // Seeded from setUtc where there is one - the spec calls for exactly that,
    // and it is an honest timestamp rather than an invented one. It does not
    // win against a peer that has changed something since, which is right:
    // "the anchor went down at T" should lose to "and then I armed it at T+1".
    if (appConfig.cfg.anchorSet && appConfig.cfg.anchorRevUtc == 0) {
        if (appConfig.cfg.anchorSetUtc) {
            appConfig.cfg.anchorRevUtc  = appConfig.cfg.anchorSetUtc;
            appConfig.cfg.anchorRevNode = ANCHOR_NODE_PINNACE;
            Serial.printf("[anchorsync] seeded the missing revision from the time the "
                          "anchor went down (%lu)\n",
                          (unsigned long)appConfig.cfg.anchorRevUtc);
        } else {
            // No set time either - an anchor from before this firmware carried
            // any of these fields. Nothing honest is available, so the state is
            // claimed on first contact, when the peer's revision is known and
            // can be beaten. Asserting costs the peer a state it may think
            // current; adopting would silently disarm a live drag alarm. The
            // dangerous direction decides.
            s_claimPending = true;
            Serial.println("[anchorsync] anchor is down with neither a revision nor a "
                           "set time - it will be claimed against the first state "
                           "heard on the bus.");
        }
    }
    if (!canSend) {
        Serial.println("[anchorsync] TX disabled (listen-only): the anchor state "
                       "is received but never sent, so changes made HERE do not "
                       "reach NauticPi.");
    }
    // First heartbeat right away, so a device that just booted is agreed with
    // rather than waiting out a full period.
    s_txPending = true;
}

void loop() {
    if (!s_canSend) return;
    const uint32_t now = millis();
    if (s_txPending || (now - s_lastTxMs) >= ANCHOR_SYNC_PERIOD_MS) {
        s_txPending = false;
        broadcast();
    }
}

void onMessage(const tN2kMsg &msg) {
#ifdef SIMULATOR
    (void)msg;                       // no bus on the PC
#else
    AnchorSyncState in;
    if (!anchorSyncDecode(msg.Data, (size_t)msg.DataLen, in)) return;

    // First peer message since boot, logged once. Two devices on different PGNs
    // look exactly like two devices in perfect agreement - silence either way -
    // and that is how the last PGN mix-up stayed hidden. This line is the
    // difference between "nothing to do" and "nothing is arriving".
    if (!s_peerSeen) {
        s_peerSeen = true;
        Serial.printf("[anchorsync] peer heard on PGN %lu: node %u rev=%lu armed=%d pos=%d\n",
                      (unsigned long)ANCHOR_SYNC_PGN, in.revNode,
                      (unsigned long)in.revUtc, in.armed, in.posValid);
    }

    // There is deliberately NO "ignore revNode == 2" filter here. It looks like
    // the obvious echo guard and it is a trap: rule 1 says the adopter keeps
    // revUtc AND revNode unchanged, so after any change made HERE, NauticPi
    // legitimately holds and heartbeats a state stamped node 2. Dropping those
    // would throw away the only messages that can bring this device back into
    // agreement after it loses its config - which is exactly when it needs
    // them. A genuine echo of our own broadcast carries our own revUtc and our
    // own revNode, so arbitration ignores it anyway: not newer, and not a lower
    // node. The filter was redundant where it was right and harmful where it
    // was not.

    // One-time claim on an anchor this device was already watching before it
    // had any revision to compare - see begin(). Asserting it costs NauticPi a
    // state it may consider current; adopting would silently disarm a live drag
    // alarm on a boat at anchor. The dangerous direction decides.
    if (s_claimPending) {
        s_claimPending = false;
        appConfig.cfg.anchorRevUtc =
            anchorSyncStampRevision(anchorSyncNowUtcOrZero(), in.revUtc);
        appConfig.cfg.anchorRevNode = ANCHOR_NODE_PINNACE;
        s_txPending = true;
        Serial.printf("[anchorsync] claiming the anchor this device already held "
                      "(no stored revision) over node %u rev=%lu -> rev=%lu\n",
                      in.revNode, (unsigned long)in.revUtc,
                      (unsigned long)appConfig.cfg.anchorRevUtc);
        return;
    }

    // Cheap pre-filter only. The binding decision is taken again in
    // applyPending(), against the state that is actually there by then.
    if (!anchorSyncShouldAdopt(in, localState())) return;

    ANCHOR_LOCK();
    s_pend    = in;
    s_hasPend = true;
    ANCHOR_UNLOCK();
#endif
}

void noteLocalChange() {
    auto &c = appConfig.cfg;
    c.anchorRevUtc  = anchorSyncStampRevision(anchorSyncNowUtcOrZero(), c.anchorRevUtc);
    c.anchorRevNode = ANCHOR_NODE_PINNACE;
    s_txPending     = true;
}

bool applyPending() {
    AnchorSyncState in;
    bool have = false;
    ANCHOR_LOCK();
    if (s_hasPend) { in = s_pend; s_hasPend = false; have = true; }
    ANCHOR_UNLOCK();
    if (!have) return false;

    // ARBITRATE AGAIN, HERE. onMessage() decided on the bus task against the
    // state as it was at that instant; between then and now the user may have
    // pressed a button. main.cpp runs this BEFORE lv_timer_handler(), so every
    // local change in a frame lands after it and would be overwritten a frame
    // later by a state that no longer wins. That is not a cosmetic race: it
    // undoes the user's own tap, and because the peer has meanwhile adopted
    // our stamp - keeping revNode 2 per rule 1 - both devices then sit on
    // states neither will give up. Deciding once more, against what is really
    // there, costs one comparison.
    if (!anchorSyncShouldAdopt(in, localState())) return false;

    auto &c = appConfig.cfg;
    const bool changed =
        c.anchorSet     != in.posValid ||
        c.anchorAlarmOn != in.armed    ||
        c.anchorSetUtc  != in.setUtc   ||
        fabsf(c.anchorRadius - in.radiusM) > 0.05f ||
        (in.posValid && (isnan(c.anchorLat) || isnan(c.anchorLon) ||
                         fabs((double)c.anchorLat - in.lat) > 1e-7 ||
                         fabs((double)c.anchorLon - in.lon) > 1e-7));

    c.anchorSet     = in.posValid;
    c.anchorAlarmOn = in.armed;
    c.anchorSetUtc  = in.setUtc;
    if (in.posValid) { c.anchorLat = (float)in.lat; c.anchorLon = (float)in.lon; }
    else             { c.anchorLat = NAN;           c.anchorLon = NAN;           }
    if (in.radiusM > 0.f) c.anchorRadius = in.radiusM;

    // Taken over UNCHANGED. Stamping our own revision here would make the
    // foreign state look like our own change, we would send it back, and the
    // two devices would push the anchor to and fro forever.
    c.anchorRevUtc  = in.revUtc;
    c.anchorRevNode = in.revNode;

    appConfig.save();
    Serial.printf("[anchorsync] adopted from node %u: armed=%d pos=%d "
                  "%.7f/%.7f r=%.1f rev=%lu set=%lu\n",
                  in.revNode, in.armed, in.posValid, in.lat, in.lon,
                  in.radiusM, (unsigned long)in.revUtc,
                  (unsigned long)in.setUtc);
    return changed;
}

}  // namespace AnchorSyncNode
