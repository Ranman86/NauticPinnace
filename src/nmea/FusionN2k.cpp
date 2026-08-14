#include "FusionN2k.h"
#include "DataModel.h"
#include <string.h>

#ifndef SIMULATOR
#include <N2kMsg.h>     // tN2kMsg – not available in the PC simulator build
#endif

// ============================================================
// Fusion media control + status over NMEA 2000.
//
// ⚠ OPCODES TO VERIFY ON HARDWARE ⚠
// The Fusion command/status opcodes below are best-effort from the canboat /
// SignalK reverse-engineering of the Fusion proprietary PGNs. They are isolated
// here as named constants so they can be confirmed/adjusted against a real radio
// once the CAN bus is live. The message FRAMING (PGN, Fusion manufacturer
// header, fast-packet) is standard and correct; only the command/message IDs and
// the exact field offsets may need tweaking per firmware.
// ============================================================
namespace Fusion {

bool    n2kActive     = false;
uint8_t deviceAddress = 0xFF;   // radio's N2K source address (learned from RX)

static void (*s_send)(const tN2kMsg &) = nullptr;
void setSendHook(void (*fn)(const tN2kMsg &)) { s_send = fn; }

// ---- protocol constants (VERIFY) -------------------------------------------
// Fusion proprietary header: manufacturer 419 + marine industry (4) + reserved.
//   value = 419 | (3<<11) | (4<<13) = 0x99A3  → little-endian bytes A3 99
static const uint8_t  FUSION_MFG_LSB = 0xA3;
static const uint8_t  FUSION_MFG_MSB = 0x99;
static const uint16_t FUSION_MFG_ID  = 419;

static const uint32_t PGN_FUSION_CMD    = 126720UL;  // command TO the radio
static const uint32_t PGN_FUSION_STATUS = 130820UL;  // status FROM the radio

// PGN 126720 "Proprietary ID" (16-bit, after the 2-byte mfg header) — canboat.
static const uint16_t CMD_REQUEST_STATUS = 1;    // no params
static const uint16_t CMD_SET_SOURCE     = 2;    // params: [sourceId]
static const uint16_t CMD_MEDIA_CONTROL  = 3;    // params: [sourceId][command]
static const uint16_t CMD_SET_MUTE       = 17;   // params: [1=on/2=off]
static const uint16_t CMD_SET_VOLUME     = 24;   // params: [zone][volume]
// FUSION_COMMAND enum (media-control action):
static const uint8_t TRANSPORT_PLAY  = 1;
static const uint8_t TRANSPORT_PAUSE = 2;
static const uint8_t TRANSPORT_NEXT  = 4;
static const uint8_t TRANSPORT_PREV  = 6;

// PGN 130820 "Message ID" (16-bit, after the 2-byte mfg header) — canboat.
static const uint16_t MSG_SOURCE       = 32770;  // sourceId + curSrc + type + flags + name
static const uint16_t MSG_SOURCE_COUNT = 32771;  // source count
static const uint16_t MSG_MEDIA        = 32772;  // srcId + flags + track# + count + length + pos
static const uint16_t MSG_TRACK_TITLE  = 32773;  // srcId + index + name
static const uint16_t MSG_TRACK_ARTIST = 32774;
static const uint16_t MSG_TRACK_ALBUM  = 32775;
static const uint16_t MSG_TRACK_POS    = 32777;  // srcId + 24-bit progress (ms)
static const uint16_t MSG_MUTE         = 32791;  // 1=on
static const uint16_t MSG_VOLUMES      = 32797;  // zone1..4 volume

// Fusion volume wire range (native steps). UI uses 0..100 and maps here.
static const int FUSION_VOL_MAX = 24;

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline uint8_t volTo100(int wire)  { return (uint8_t)clampi(wire * 100 / FUSION_VOL_MAX, 0, 100); }
static inline uint8_t volToWire(int pct)  { return (uint8_t)clampi(pct * FUSION_VOL_MAX / 100, 0, FUSION_VOL_MAX); }

// ---- low-level command TX ---------------------------------------------------
#ifndef SIMULATOR
// Build a Fusion PGN 126720 command and send it (fast-packet handled by the lib).
// The Proprietary ID is a 16-bit field (canboat) — sent little-endian.
static void txCommand(uint16_t propId, const uint8_t *params, uint8_t nParams) {
    if (!s_send) return;
    tN2kMsg m;
    m.SetPGN(PGN_FUSION_CMD);
    m.Priority    = 7;
    m.Destination = deviceAddress;
    m.AddByte(FUSION_MFG_LSB);
    m.AddByte(FUSION_MFG_MSB);
    m.Add2ByteUInt(propId);                      // 16-bit proprietary ID
    for (uint8_t i = 0; i < nParams; i++) m.AddByte(params[i]);
    s_send(m);
}
#else
static void txCommand(uint16_t, const uint8_t *, uint8_t) {}  // no-op in simulator
#endif

static void txSetZoneVolumeWire(int zone, int pct) {
    uint8_t p[2] = { (uint8_t)zone, volToWire(pct) };
    txCommand(CMD_SET_VOLUME, p, 2);
}

// ---- control (called from the Fusion screen; updates DataModel immediately) -
void setSource(int idx) {
    {
        auto lk = data.lock();
        if (idx >= 0 && idx < (int)data.fusionSourceCount) {
            data.fusionSource = (int8_t)idx;
            data.lastFusionUpdate = millis();
        }
    }
    if (n2kActive) { uint8_t p = (uint8_t)idx; txCommand(CMD_SET_SOURCE, &p, 1); }
}

void cycleSource(int dir) {
    int next;
    {
        auto lk = data.lock();
        if (data.fusionSourceCount == 0) return;
        int cur = data.fusionSource < 0 ? 0 : data.fusionSource;
        next = (cur + dir + data.fusionSourceCount) % data.fusionSourceCount;
    }
    setSource(next);
}

void setZoneVolume(int zone, int vol) {
    if (zone < 0 || zone >= DataModel::FUSION_NUM_ZONES) return;
    vol = clampi(vol, 0, 100);
    {
        auto lk = data.lock();
        data.fusionZoneVol[zone]  = (uint8_t)vol;
        data.fusionZoneMute[zone] = false;   // dragging the level unmutes the zone
        data.lastFusionUpdate = millis();
    }
    if (n2kActive) txSetZoneVolumeWire(zone, vol);
}

void setMasterVolume(int vol) {
    vol = clampi(vol, 0, 100);
    uint8_t newVals[DataModel::FUSION_NUM_ZONES];
    {
        auto lk = data.lock();
        int old = data.fusionMasterVol();   // loudest zone = reference
        for (int i = 0; i < DataModel::FUSION_NUM_ZONES; i++) {
            int nv = (old <= 0) ? vol : clampi((int)(data.fusionZoneVol[i] * (float)vol / old + 0.5f), 0, 100);
            data.fusionZoneVol[i] = (uint8_t)nv;
            newVals[i] = data.fusionZoneMute[i] ? 0 : (uint8_t)nv;   // muted zones stay silent
        }
        data.lastFusionUpdate = millis();
    }
    if (n2kActive) for (int i = 0; i < DataModel::FUSION_NUM_ZONES; i++) txSetZoneVolumeWire(i, newVals[i]);
}

void nudgeMaster(int delta) {
    int m;
    { auto lk = data.lock(); m = data.fusionMasterVol(); }
    setMasterVolume(m + delta);
}

// Per-zone mute: keeps the zone's set volume, sends 0 while muted. The slider
// keeps showing the level; a mute icon marks the state; dragging the level (in
// setZoneVolume) unmutes.
void toggleZoneMute(int zone) {
    if (zone < 0 || zone >= DataModel::FUSION_NUM_ZONES) return;
    bool m; int vol;
    {
        auto lk = data.lock();
        data.fusionZoneMute[zone] = !data.fusionZoneMute[zone];
        m   = data.fusionZoneMute[zone];
        vol = data.fusionZoneVol[zone];
        data.lastFusionUpdate = millis();
    }
    if (n2kActive) txSetZoneVolumeWire(zone, m ? 0 : vol);
}

// Master mute: if any zone is muted → unmute all, else mute all.
void toggleAllMute() {
    uint8_t eff[DataModel::FUSION_NUM_ZONES];
    {
        auto lk = data.lock();
        bool any = false;
        for (int i = 0; i < DataModel::FUSION_NUM_ZONES; i++) if (data.fusionZoneMute[i]) any = true;
        bool nm = !any;
        for (int i = 0; i < DataModel::FUSION_NUM_ZONES; i++) {
            data.fusionZoneMute[i] = nm;
            eff[i] = nm ? 0 : data.fusionZoneVol[i];
        }
        data.lastFusionUpdate = millis();
    }
    if (n2kActive) for (int i = 0; i < DataModel::FUSION_NUM_ZONES; i++) txSetZoneVolumeWire(i, eff[i]);
}

void playPause() {
    uint8_t newState; int8_t src;
    {
        auto lk = data.lock();
        data.fusionPlayState = (data.fusionPlayState == 1) ? 2 : 1;
        newState = data.fusionPlayState; src = data.fusionSource;
        data.lastFusionUpdate = millis();
    }
    if (n2kActive) {
        uint8_t p[2] = { (uint8_t)(src < 0 ? 0 : src),
                         (uint8_t)(newState == 1 ? TRANSPORT_PLAY : TRANSPORT_PAUSE) };
        txCommand(CMD_MEDIA_CONTROL, p, 2);
    }
}

// In demo mode next/prev jump the demo track list; live they send transport cmds.
void nextTrack();   // fwd (demo impl below)
void prevTrack();

// ---- incoming status (PGN 130820) -------------------------------------------
#ifndef SIMULATOR
static void copyStr(char *dst, size_t dstSz, const tN2kMsg &msg, int &idx) {
    // Fusion strings: 1 length byte then ASCII. Defensive bounds.
    uint8_t len = msg.GetByte(idx);
    size_t n = 0;
    for (uint8_t i = 0; i < len; i++) {
        unsigned char c = msg.GetByte(idx);
        if (n + 1 < dstSz && c >= 0x20 && c < 0x7F) dst[n++] = (char)c;
    }
    dst[n] = '\0';
}

void handlePGN130820(const tN2kMsg &msg) {
    int idx = 0;
    uint16_t mfg = msg.Get2ByteUInt(idx);
    if ((mfg & 0x07FF) != FUSION_MFG_ID) return;       // not a Fusion message
    uint16_t messageId = msg.Get2ByteUInt(idx);        // 16-bit message ID (was 8!)

    // A raw hex dump of incoming 130820 frames used to live here — it is how the
    // opcodes below were reverse engineered. Removed for release: it printed on
    // every frame from the radio. Re-add it locally (guarded by a build flag)
    // when validating the opcodes against real hardware.

    deviceAddress = msg.Source;
    auto lk = data.lock();
    data.fusionConnected  = true;
    data.lastFusionUpdate = millis();
    switch (messageId) {
        case MSG_SOURCE: {
            uint8_t sid = msg.GetByte(idx);   // Source ID (this source)
            uint8_t cur = msg.GetByte(idx);   // Current Source ID (the ACTIVE one)
            msg.GetByte(idx);                 // Source Type
            msg.GetByte(idx);                 // Flags
            char name[16]; copyStr(name, sizeof(name), msg, idx);
            if (sid < DataModel::FUSION_MAX_SOURCES && name[0]) {
                strncpy(data.fusionSourceName[sid], name, sizeof(data.fusionSourceName[0]) - 1);
                data.fusionSourceName[sid][sizeof(data.fusionSourceName[0]) - 1] = 0;
                if (sid + 1 > data.fusionSourceCount) data.fusionSourceCount = sid + 1;
            }
            if (cur < DataModel::FUSION_MAX_SOURCES) data.fusionSource = (int8_t)cur;  // active source
            break;
        }
        case MSG_SOURCE_COUNT: {
            uint8_t cnt = msg.GetByte(idx);
            if (cnt <= DataModel::FUSION_MAX_SOURCES) data.fusionSourceCount = cnt;
            break;
        }
        case MSG_MEDIA: {
            msg.GetByte(idx);                 // Source ID
            msg.Get2ByteUInt(idx);            // Flags (play state)
            msg.Get4ByteUInt(idx);            // Track #
            msg.Get4ByteUInt(idx);            // Track Count
            uint32_t lengthMs = msg.Get4ByteUInt(idx);  // Length,   res 0.001 s → ms
            uint32_t posMs    = msg.Get4ByteUInt(idx);  // Position, res 0.001 s → ms
            if (lengthMs != 0xFFFFFFFF) data.fusionTotalMs   = lengthMs;
            if (posMs    != 0xFFFFFFFF) data.fusionElapsedMs = posMs;
            break;
        }
        case MSG_TRACK_TITLE:
            msg.GetByte(idx); msg.Get4ByteUInt(idx);    // Source ID + Index
            copyStr(data.fusionTitle, sizeof(data.fusionTitle), msg, idx);
            break;
        case MSG_TRACK_ARTIST:
            msg.GetByte(idx); msg.Get4ByteUInt(idx);
            copyStr(data.fusionArtist, sizeof(data.fusionArtist), msg, idx);
            break;
        case MSG_TRACK_ALBUM:
            msg.GetByte(idx); msg.Get4ByteUInt(idx);
            copyStr(data.fusionAlbum, sizeof(data.fusionAlbum), msg, idx);
            break;
        case MSG_TRACK_POS: {
            msg.GetByte(idx);                             // Source ID
            uint32_t p = msg.Get3ByteUInt(idx);           // 24-bit progress, res 0.001 s → ms
            if (p != 0xFFFFFF) data.fusionElapsedMs = p;
            break;
        }
        case MSG_VOLUMES:
            for (int z = 0; z < DataModel::FUSION_NUM_ZONES; z++)
                data.fusionZoneVol[z] = volTo100(msg.GetByte(idx));
            break;
        case MSG_MUTE:
            // The radio's global mute flag — the UI works only with the
            // per-zone mutes (fusionZoneMute), so nothing to store here.
            break;
        default: break;
    }
}
#endif // !SIMULATOR

void requestStatus() {
    if (n2kActive) txCommand(CMD_REQUEST_STATUS, nullptr, 0);
}

// ============================================================
// Demo data (used when demoMode / CAN off): a fake Fusion that plays a
// playlist so the screen is fully alive and interactive in the simulator.
// ============================================================
static const char *DEMO_SOURCES[] = { "FM", "AM", "AUX", "Bluetooth", "USB" };
static const int   DEMO_SOURCE_N  = 5;
struct DemoTrack { const char *title, *artist, *album; uint16_t lenS; };
static const DemoTrack DEMO_TRACKS[] = {
    { "Sailing",            "Rod Stewart", "Atlantic Crossing", 230 },
    { "The Ocean",          "Led Zeppelin","Houses of the Holy", 271 },
    { "Orinoco Flow",       "Enya",        "Watermark",          265 },
    { "Beyond the Sea",     "Bobby Darin", "That's All",         173 },
    { "La Mer",             "Charles Trenet","Le Soleil et la Lune", 200 },
};
static const int DEMO_TRACK_N = 5;
static int      s_demoTrack   = 0;
static uint32_t s_demoLastMs  = 0;

static void demoLoadTrack(int i) {
    auto lk = data.lock();
    const DemoTrack &t = DEMO_TRACKS[i % DEMO_TRACK_N];
    strncpy(data.fusionTitle,  t.title,  sizeof(data.fusionTitle)  - 1);  data.fusionTitle [sizeof(data.fusionTitle)-1]=0;
    strncpy(data.fusionArtist, t.artist, sizeof(data.fusionArtist) - 1);  data.fusionArtist[sizeof(data.fusionArtist)-1]=0;
    strncpy(data.fusionAlbum,  t.album,  sizeof(data.fusionAlbum)  - 1);  data.fusionAlbum [sizeof(data.fusionAlbum)-1]=0;
    data.fusionElapsedMs = 0;
    data.fusionTotalMs   = (uint32_t)t.lenS * 1000;
}

void demoInit() {
    {
        auto lk = data.lock();
        data.fusionSourceCount = DEMO_SOURCE_N;
        for (int i = 0; i < DEMO_SOURCE_N; i++) {
            strncpy(data.fusionSourceName[i], DEMO_SOURCES[i], sizeof(data.fusionSourceName[0]) - 1);
            data.fusionSourceName[i][sizeof(data.fusionSourceName[0]) - 1] = 0;
        }
        data.fusionSource    = 3;     // Bluetooth
        data.fusionZoneVol[0]= 65;    // Zone 1 (Saloon)
        data.fusionZoneVol[1]= 45;    // Zone 2 (Cockpit)
        data.fusionZoneVol[2]= 20;    // Zone 3 (Cabin)
        for (int i = 0; i < DataModel::FUSION_NUM_ZONES; i++) data.fusionZoneMute[i] = false;
        data.fusionPlayState = 1;     // playing
        data.fusionConnected = true;
    }
    demoLoadTrack(s_demoTrack);
    s_demoLastMs = millis();
}

void demoTick(uint32_t nowMs) {
    if (s_demoLastMs == 0) s_demoLastMs = nowMs;
    uint32_t dt = nowMs - s_demoLastMs;
    s_demoLastMs = nowMs;
    auto lk = data.lock();
    if (data.fusionPlayState != 1) return;             // only advance while playing
    data.fusionElapsedMs += dt;
    if (data.fusionTotalMs && data.fusionElapsedMs >= data.fusionTotalMs) {
        s_demoTrack = (s_demoTrack + 1) % DEMO_TRACK_N;
        const DemoTrack &t = DEMO_TRACKS[s_demoTrack];
        strncpy(data.fusionTitle,  t.title,  sizeof(data.fusionTitle)  - 1);  data.fusionTitle [sizeof(data.fusionTitle)-1]=0;
        strncpy(data.fusionArtist, t.artist, sizeof(data.fusionArtist) - 1);  data.fusionArtist[sizeof(data.fusionArtist)-1]=0;
        strncpy(data.fusionAlbum,  t.album,  sizeof(data.fusionAlbum)  - 1);  data.fusionAlbum [sizeof(data.fusionAlbum)-1]=0;
        data.fusionElapsedMs = 0;
        data.fusionTotalMs   = (uint32_t)t.lenS * 1000;
    }
}

void nextTrack() {
    if (n2kActive) {
        int8_t src; { auto lk = data.lock(); src = data.fusionSource; }
        uint8_t p[2] = { (uint8_t)(src < 0 ? 0 : src), TRANSPORT_NEXT };
        txCommand(CMD_MEDIA_CONTROL, p, 2); return;
    }
    s_demoTrack = (s_demoTrack + 1) % DEMO_TRACK_N;
    demoLoadTrack(s_demoTrack);
}

void prevTrack() {
    if (n2kActive) {
        int8_t src; { auto lk = data.lock(); src = data.fusionSource; }
        uint8_t p[2] = { (uint8_t)(src < 0 ? 0 : src), TRANSPORT_PREV };
        txCommand(CMD_MEDIA_CONTROL, p, 2); return;
    }
    s_demoTrack = (s_demoTrack - 1 + DEMO_TRACK_N) % DEMO_TRACK_N;
    demoLoadTrack(s_demoTrack);
}

} // namespace Fusion
