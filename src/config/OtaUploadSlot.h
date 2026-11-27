#pragma once
// ============================================================================
// OtaUploadSlot - which WiFi upload (/api/ota) owns the update writers, and
// when a new upload may take them over (docs/N2K-UPDATE.md 11.5).
//
// A client that drops off the network without closing its connection (phone
// out of range, WiFi switched off, lid shut) sends nothing more, and its
// request ends only when the server's receive timeout runs out: 20 s
// (OTA_RX_TIMEOUT_S, WebConfig.cpp). Until then it held the writers. An upload
// started in that window went over the air in full for nothing - the server
// can answer only once the whole body is in - and was then refused as busy,
// with a text that blamed NMEA 2000.
//
// The rules:
//   - A new upload may take the writers over from an owner that has sent
//     nothing for OTA_TAKEOVER_MS. Time alone never ends an upload: only a
//     new upload takes over, and only from a silent one.
//   - "Sent nothing" counts from the end of the owner's last write, so a slow
//     flash erase or file write is not taken for the client's silence.
//   - Never two writers on flash at once: the silent owner's writer is
//     aborted before the new one begins. That is decided before the new
//     writer is opened, so a new upload that then cannot begin - the web
//     UI's work region still in its 120 s grace after an install, no memory,
//     a flash error - has ended the silent one all the same, and that one's
//     client, should it come back, is told it was replaced. Left so on
//     purpose: it had sent nothing for 10 s, its own receive timeout would
//     end it 10 s later, and checking first would mean a second look into
//     the update regions' grace, which begin() does anyway.
//   - Ownership goes by number, never by pointer - a request may be gone by
//     the time anyone looks. An upload that was taken over never touches the
//     writers again: its later pieces are dropped, its reply says "replaced",
//     and its disconnect (up to 20 s later) aborts nothing.
//
// Only the async_tcp task uses this - every ESPAsyncWebServer handler runs
// there - so there is no lock. Plain C++ without Arduino calls, also C++11
// (the 4" core): the host test compiles this header as it is, with fake
// writers and a fake clock behind the Env interface below. WebConfig.cpp
// only dispatches to it.
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include "../n2kupdate/N2kUpdateProto.h"   // n2kupd::RSN_BUSY

// How long the owner must have been silent before a new upload may take
// over. Not a cut-off: an upload stalled for longer goes on (the server's
// receive timeout ends it after 20 s) unless a new upload starts meanwhile.
// Weak links have shown stalls of 3 s and more, hence not shorter.
static const uint32_t OTA_TAKEOVER_MS = 10000;

enum : uint8_t { OTA_TARGET_FW = 1, OTA_TARGET_WEBUI = 2 };

// With RSN_BUSY: what the reply names as "gate".
enum : uint8_t {
    OTA_BUSY_GATE     = 0,   // UpdateGate::describe() at reply time
    OTA_BUSY_WIFI     = 1,   // "wifi": another WiFi upload has the writers
    OTA_BUSY_REPLACED = 2,   // "replaced": a newer upload took this one's over
    OTA_BUSY_GRACE    = 3,   // "grace": the writer's begin() refused - the web
                             // UI's work region is still in its 120 s grace
                             // after the last web UI install (UpdRegions.h)
};

// Per request, in request->_tempObject: plain data, because the request's
// destructor free()s that slot. Zeroed when the first piece comes in.
struct OtaUpload {
    uint8_t  target;          // OTA_TARGET_*, 0 = refused target
    uint8_t  reason;          // 0 = fine so far, else a reason code (4.8)
    uint8_t  detail;          // web UI: 1 = served from the next restart
    bool     started;         // the writer was opened for this upload - the
                              // slot tells whether it still owns it
    bool     finished;        // installed
    uint8_t  busyGate;        // with RSN_BUSY: OTA_BUSY_*
    uint32_t retrySeconds;    // with RSN_BUSY and OTA_BUSY_GATE or OTA_BUSY_GRACE
    uint32_t id;              // its number in OtaUploadSlot, 0 = none
};

struct OtaUploadSlot {
    uint32_t owner    = 0;   // number of the upload that owns the writers, 0 = none
    uint32_t lastId   = 0;
    uint32_t lastRxMs = 0;   // when the owner's last piece was written
    uint8_t  target   = 0;   // the owner's OTA_TARGET_*

    // A new owner. Its number is never 0, also when the counter wraps.
    uint32_t claim(uint8_t t, uint32_t now) {
        if (++lastId == 0) lastId = 1;
        owner = lastId;
        target = t;
        lastRxMs = now;
        return owner;
    }
    bool owns(uint32_t id) const { return id != 0 && id == owner; }
    // How long the owner has been silent; 0 without one.
    uint32_t quietMs(uint32_t now) const { return owner ? (uint32_t)(now - lastRxMs) : 0; }
    // Seconds until a new upload may take over, rounded up: 0 = now, or
    // nobody holds the writers.
    uint32_t takeoverInS(uint32_t now) const {
        const uint32_t q = quietMs(now);
        if (!owner || q >= OTA_TAKEOVER_MS) return 0;
        return (OTA_TAKEOVER_MS - q + 999) / 1000;
    }
    // Frees the slot if the owner has been silent for OTA_TAKEOVER_MS and
    // returns its target - its writer is the caller's to abort - else 0.
    uint8_t takeOverIfQuiet(uint32_t now) {
        if (!owner || quietMs(now) < OTA_TAKEOVER_MS) return 0;
        owner = 0;
        return target;
    }
    enum ChunkAct : uint8_t { CH_WRITE, CH_REPLACED };
    // A piece of upload `id` came in: write it, or drop it because a newer
    // upload has taken the writers over.
    ChunkAct onChunk(uint32_t id) const { return owns(id) ? CH_WRITE : CH_REPLACED; }
    // The owner's piece is written: from now on its silence counts.
    void wrote(uint32_t id, uint32_t now) { if (owns(id)) lastRxMs = now; }
    // Upload `id` is going away. True if it still owned the writers - the
    // caller aborts its writer then - and false once taken over.
    bool abortOnDisconnect(uint32_t id) {
        if (!owns(id)) return false;
        owner = 0;
        return true;
    }
    // Upload `id` is done with the writers: finished, refused, or its writer
    // aborted itself.
    void onEnd(uint32_t id) { if (owns(id)) owner = 0; }
};

// ---- the steps of an upload -----------------------------------------------------
// Env - the writers, the gate and the clock (WebConfig.cpp: OtaEnv):
//   uint32_t now()                                    millis()
//   bool     canStart()                               UpdateGate::canStartWifiOta()
//   uint32_t gateRetryS()                             UpdateGate::restartBlockedSeconds()
//   uint8_t  begin(uint8_t target, uint32_t &retryS)  open the target's writer, a reason code;
//                                                     RSN_BUSY only from the web UI's (its work
//                                                     region's grace), retryS = the rest
//   uint8_t  write(uint8_t target, const uint8_t *p, size_t n)
//                                                     a reason code; aborts itself on failure
//   uint8_t  finish(uint8_t target, uint8_t &detail)  install; releases the writer either way
//   void     abort(uint8_t target)                    abort and release the target's writer

// The first piece of an upload whose target is known (u->target set, the
// rest zeroed): takes the writers over from a silent owner, then opens the
// writer or refuses with RSN_BUSY. True when it took the writers over.
template <class Env>
bool otaUploadStart(OtaUploadSlot &s, OtaUpload *u, Env &env) {
    const uint8_t old = s.takeOverIfQuiet(env.now());
    if (old) env.abort(old);                  // before anything begins
    if (!env.canStart()) {
        u->reason = n2kupd::RSN_BUSY;
        // An owner left means another WiFi upload, heard from within
        // OTA_TAKEOVER_MS; the reply tells how long until it may be taken over.
        if (s.owner) u->busyGate = OTA_BUSY_WIFI;
        else         u->retrySeconds = env.gateRetryS();
    } else {
        u->reason = env.begin(u->target, u->retrySeconds);
        // Only the web UI's begin() answers busy: its work region, retired by
        // the last web UI install, is still in its grace (retrySeconds = the
        // rest). Its own gate - "idle", what UpdateGate says then, made the
        // web interface blame an update over NMEA 2000.
        if (u->reason == n2kupd::RSN_BUSY) u->busyGate = OTA_BUSY_GRACE;
    }
    if (!u->reason) {
        u->started = true;
        u->id = s.claim(u->target, env.now());
    }
    return old != 0;
}

enum OtaPiece : uint8_t {
    OTA_PIECE_IGNORED,    // not started, refused, or already over
    OTA_PIECE_REPLACED,   // taken over by a newer upload: this and the rest dropped
    OTA_PIECE_WRITTEN,
    OTA_PIECE_FAILED,     // the writer refused it and aborted itself (u->reason)
    OTA_PIECE_DONE,       // the last piece: installed or refused (u->reason)
};

// Every piece of an upload, the first one included (after otaUploadStart).
template <class Env>
OtaPiece otaUploadPiece(OtaUploadSlot &s, OtaUpload *u, Env &env,
                        const uint8_t *data, size_t len, bool final) {
    if (!u || !u->started) return OTA_PIECE_IGNORED;
    if (s.onChunk(u->id) == OtaUploadSlot::CH_REPLACED) {
        u->started = false;
        u->reason = n2kupd::RSN_BUSY;
        u->busyGate = OTA_BUSY_REPLACED;
        return OTA_PIECE_REPLACED;
    }
    if (len) {
        const uint8_t r = env.write(u->target, data, len);
        if (r) {
            s.onEnd(u->id);
            u->reason = r;
            u->started = false;
            return OTA_PIECE_FAILED;
        }
    }
    s.wrote(u->id, env.now());                // after the write, see the rules above
    if (!final) return OTA_PIECE_WRITTEN;
    const uint8_t r = env.finish(u->target, u->detail);
    s.onEnd(u->id);
    u->reason = r;
    u->started = false;
    u->finished = r == 0;
    return OTA_PIECE_DONE;
}

// The request is going away (its disconnect), or its first piece came again:
// abort its writer - unless a newer upload has taken it over. True when a
// writer was aborted.
template <class Env>
bool otaUploadRelease(OtaUploadSlot &s, OtaUpload *u, Env &env) {
    if (!u || !u->started) return false;
    u->started = false;
    if (!s.abortOnDisconnect(u->id)) return false;
    env.abort(u->target);
    return true;
}

// A BUSY reply: its "gate" and its "seconds". For "wifi" the seconds until a
// new upload may take over, counted when the reply goes out - the refused
// upload's own body took its time to arrive. For "grace" (and the gate's own
// refusals) they are counted when the upload was refused, so they can read a
// little long by then, never short.
inline const char *otaBusyGate(const OtaUpload &u, const char *gateNow) {
    return u.busyGate == OTA_BUSY_REPLACED ? "replaced"
         : u.busyGate == OTA_BUSY_WIFI     ? "wifi"
         : u.busyGate == OTA_BUSY_GRACE    ? "grace" : gateNow;
}
inline uint32_t otaBusySeconds(const OtaUpload &u, const OtaUploadSlot &s, uint32_t now) {
    return u.busyGate == OTA_BUSY_REPLACED ? 0
         : u.busyGate == OTA_BUSY_WIFI     ? s.takeoverInS(now) : u.retrySeconds;
}
