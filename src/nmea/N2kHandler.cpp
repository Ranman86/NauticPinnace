#include "N2kHandler.h"
#include "AnchorSyncNode.h"
#include "DemoData.h"
#include "MediaN2k.h"
#include "WindPipeline.h"
#include "ApPipeline.h"
#include "N2kTx.h"
#include "../BoardConfig.h"
#include "../Version.h"
#include "../config/Config.h"
#include "../n2kupdate/N2kUpdateNode.h"
#include "../n2kupdate/N2kUpdateProto.h"
#include "../n2kupdate/OtaGuard.h"
#include "../n2kupdate/UpdateGate.h"
#include <N2kMessages.h>
#include <esp_mac.h>
#include "driver/twai.h"   // twai_clear_receive_queue() when the demo ends

// Use custom TX/RX pins before including NMEA2000_CAN.h
#define ESP32_CAN_TX_PIN (gpio_num_t)CAN_TX_PIN
#define ESP32_CAN_RX_PIN (gpio_num_t)CAN_RX_PIN
#include <NMEA2000_CAN.h>

// ---- N2K source selection & statistics --------------------------------------
// Several bus devices can send the same data (two GPS, two compasses...).
// Every handler for a selectable category calls srcGate() first: it records
// the sender (source address + message count + last-seen, for GET
// /api/sources) and then accepts the message only when no source is pinned
// in the config or the pinned one matches. Bookkeeping happens for REJECTED
// senders too, so the WebUI can list every candidate.
N2kSrcSeen g_n2kSrcSeen[N2K_SRC_CAT_N][N2K_SRC_SLOTS] = {};

static bool srcGate(int cat, const tN2kMsg &msg, int16_t pinned) {
    N2kSrcSeen *row = g_n2kSrcSeen[cat];
    int freeSlot = -1;
    bool found = false;
    for (int i = 0; i < N2K_SRC_SLOTS; i++) {
        if (row[i].count && row[i].sa == msg.Source) {
            row[i].count++;
            row[i].lastMs = millis();
            found = true;
            break;
        }
        if (!row[i].count && freeSlot < 0) freeSlot = i;
    }
    if (!found && freeSlot >= 0) {
        row[freeSlot].sa     = msg.Source;
        row[freeSlot].count  = 1;
        row[freeSlot].lastMs = millis();
    }
    return pinned < 0 || msg.Source == (uint8_t)pinned;
}

// ---- Sensor calibration helpers (see Config.h for the two semantics) --------
static inline float calNorm360(float deg) {
    while (deg >= 360.f) deg -= 360.f;
    while (deg <    0.f) deg += 360.f;
    return deg;
}
static inline float calNorm180(float deg) {
    while (deg >  180.f) deg -= 360.f;
    while (deg < -180.f) deg += 360.f;
    return deg;
}

// ---- Init -------------------------------------------------------------------

void N2kHandler::begin() {
    // ---- CAN pins: board default unless the config overrides them ----------
    // This runs inside the N2K task, long after the config file was read, so
    // it is the first point where a configured pair CAN be applied - the
    // instance itself was built during static initialisation with the
    // compile-time defaults baked in.
    //
    // Anything unusable falls back to the board pins instead of being handed
    // to the TWAI driver. A bad GPIO here does not fail loudly: the controller
    // opens happily and simply never receives a frame, which looks exactly
    // like dead hardware and is correspondingly expensive to chase.
    auto usable = [](int p) {
        if (p < 0 || p > 48)      return false;
        if (p >= 22 && p <= 25)   return false;   // do not exist on the ESP32-S3
        if (p >= 26 && p <= 37)   return false;   // SPI flash and octal PSRAM
        return true;
    };
    int  txPin = appConfig.cfg.canTxPin;
    int  rxPin = appConfig.cfg.canRxPin;
    const bool fromCfg = usable(txPin) && usable(rxPin) && txPin != rxPin;
    if (!fromCfg) {
        if (txPin != -1 || rxPin != -1)
            Serial.printf("[n2k] configured CAN pins TX=%d RX=%d are unusable - "
                          "falling back to the board defaults\n", txPin, rxPin);
        txPin = CAN_TX_PIN;
        rxPin = CAN_RX_PIN;
    }
    static_cast<tNMEA2000_esp32 &>(NMEA2000)
        .SetCANPins((gpio_num_t)txPin, (gpio_num_t)rxPin);
    Serial.printf("[n2k] CAN pins TX=%d RX=%d (%s)\n", txPin, rxPin,
                  fromCfg ? "from config" : "board default");

    // TJA1051T/3 standby pin: drive LOW to enable the transceiver.
    // If CAN_STB_PIN == -1 the pin is hardwired to GND on the PCB – nothing to do.
#if CAN_STB_PIN >= 0
    pinMode(CAN_STB_PIN, OUTPUT);
    digitalWrite(CAN_STB_PIN, LOW);
#endif

    // ---- A NAME of its own per display (N2K-UPDATE.md 11.2) ----------------
    // The unique number used to be the constant 1001001 on every display, so
    // all of them had the same NAME. Two that claimed at the same moment then
    // both kept address 42 - the library only resolves a conflict when it hears
    // the OTHER node's claim, and identical NAMEs never lose to each other -
    // and their replies collided on the bus. 21 bits of the base MAC make it
    // unique. Chartplotters list each display once more as a new device.
    uint8_t mac[6] = { 0 };
    esp_efuse_mac_get_default(mac);
    const unsigned long uniqueNo =
        (((unsigned long)mac[3] << 16) | ((unsigned long)mac[4] << 8) | mac[5]) & 0x1FFFFFUL;
    // The model serial code is what a plotter shows as the serial number, so
    // it carries the same number instead of one shared by every display.
    static char serialCode[24];
    snprintf(serialCode, sizeof(serialCode), "NAUTICPINNACE-%07lu", uniqueNo);
    NMEA2000.SetProductInformation(
        serialCode,              // manufacturer's model serial code
        100,                     // manufacturer's product code
        "NauticPinnace",        // model ID - update senders look for it (5.2)
        FW_VERSION,              // software version code - see src/Version.h
        FW_MODEL_VERSION         // model version (hardware generation)
    );
    NMEA2000.SetDeviceInformation(
        uniqueNo,                // unique device number, from the MAC
        120,                     // device function: display
        120,                     // device class: display
        2046                     // manufacturer code (private)
    );
    Serial.printf("[n2k] NAME unique number %lu\n", uniqueNo);
    // Listen-only mode: N2km_ListenOnly sends NOTHING (no address claim and
    // no heartbeat either) — the device is invisible on the bus.
    // Additionally set the TWAI controller itself to listen-only, otherwise
    // it would still drive ACK bits onto the bus (electrically passive).
    const bool listenOnly = appConfig.cfg.n2kListenOnly;
    tNMEA2000_esp32::SetHwListenOnly(listenOnly);
    NMEA2000.SetMode(listenOnly ? tNMEA2000::N2km_ListenOnly
                                : tNMEA2000::N2km_ListenAndNode, 42);
    NMEA2000.SetMsgHandler(handleMsg);
    NMEA2000.EnableForward(false);
    // Pilot mode: enter the pilot computer remembered from the last run into
    // the NAME table before the first frame arrives (ApPipeline.h). Read
    // only - it reads NVS, it sends nothing.
    ApPipeline::begin();
    NMEA2000.Open();
    Serial.printf("[n2k] mode: %s\n", listenOnly ? "LISTEN-ONLY (silent)" : "listen+node");
    if (listenOnly)
        Serial.println(
            "[n2k] Note: listen-only sends NO acknowledge bits. With only two "
            "devices on the bus (e.g. a simulator and this display) the sender "
            "never gets through and n2kRx stays 0 - without a single error "
            "counter moving, which looks like 'nothing connected'. On a test bench "
            "listen-only must be OFF; on a real bus with further nodes it need not be.");

    if (!listenOnly) {
        // Media control (manufacturer 419): route TX through this node, mark the bus live.
        // In listen-only, n2kActive stays false -> every control path in
        // MediaN2k.cpp is already gated on it and becomes a no-op.
        //
        // The media screen calls this from the LVGL task, so it must not call
        // SendMsg() itself: the library is not thread-safe, and its 126720
        // commands could interleave with the update receiver's 126720
        // messages from the same address, which breaks fast-packet framing
        // (N2K-UPDATE.md 2.3 rule 7). They go through N2kTx, which this task
        // drains in loop().
        Media::setSendHook([](const tN2kMsg &m) { N2kTx::post(m); });
        Media::n2kActive = true;
        Media::requestStatus();   // ask any radio on the bus for its state + sources
    }
    // Anchor sync with NauticPi. The PGN sits in 130816..131071, which this
    // library treats as proprietary fast packet unconditionally - on TX via
    // IsFastPacketPGN() and on RX via CheckKnownMessage(), which sets the
    // fast-packet flag before returning "unknown". ForwardMode is 0, so
    // unknown PGNs reach the message handler anyway.
    //
    // DO NOT "FIX" THIS WITH SetFastPacketMessages(). No registration is needed,
    // and that particular call would do real damage: it occupies list slot 0,
    // and the library's built-in table of standard fast packets is consulted
    // ONLY while slot 0 is empty (NMEA2000.cpp:1633). Registering the anchor PGN
    // that way would silently reduce AIS (129038/129039/129794/129809/129810)
    // and GNSS (129029) to chopped-up 8-byte single frames. If a future change
    // ever sets SetHandleOnlyKnownMessages(true), the right answer is
    // ExtendFastPacketMessages() - which keeps only ONE list pointer, so a
    // second call replaces the first.
    AnchorSyncNode::begin(!listenOnly);
    // Updates over NMEA 2000: PGN 126720 (proprietary fast packet, like the
    // anchor PGN above - nothing to register). In listen-only mode the display
    // cannot answer, so it stays invisible to the update protocol.
    N2kUpdateNode::begin(!listenOnly);
}

void N2kHandler::loop() {
    // Demo mode switched on and off again at runtime on a device that booted on
    // the bus: wipe the model once on the way out, as main.cpp's demoTask does.
    // Without it the demo values stayed - a frozen demo track on the media
    // screen with demo mode off - until the bus happened to overwrite each one,
    // and the media fields never are without a radio.
    static bool wasDemo = false;
    // Sends posted by other tasks (media control), from here on the only
    // task that calls SendMsg() (N2K-UPDATE.md 11.2).
    N2kTx::drain();
    // Demo mode stops the bus from being parsed, so switching it on takes
    // effect only once an update session or the verify window after one is
    // over (N2K-UPDATE.md 5.1 rule 6); until then the bus branch runs on.
    if (appConfig.cfg.demoMode && UpdateGate::demoAllowed()) {
        // Demo mode: inject synthetic data instead of reading the CAN bus.
        // demoData.tick() is internally rate-limited to ~5 Hz.
        demoData.tick();
        wasDemo = true;
    } else {
        if (wasDemo) {
            { auto lk = data.lock(); data.clearValues(); }
            // The controller went on queueing frames while the demo ran - up
            // to its 65, all as old as the moment the demo started. Parsed now
            // they would be stamped as new, and on a bus that has gone quiet
            // since, old values would show as current for another 10 s.
            twai_clear_receive_queue();
            wasDemo = false;
        }
        NMEA2000.ParseMessages();
        AnchorSyncNode::loop();   // anchor heartbeat + pending broadcast
    }
    // In BOTH branches: the update timers and the UI must never freeze (9.1).
    N2kUpdateNode::tick();
    // A changed remembered pilot computer goes to NVS here, in this task and
    // outside data.lock() - not from the message handler, which holds it.
    ApPipeline::persistDue();
    // Bus-off recovery: looks at the controller once a second.
    static_cast<tNMEA2000_esp32 &>(NMEA2000).ServiceBus();
}

// ---- Message dispatcher -----------------------------------------------------

// Counts every received N2k message – shows up in the [HB] log as n2kRx.
// Useful for checking wiring/bus operation without looking at the display.
volatile uint32_t g_n2kRxCount = 0;

void N2kHandler::handleMsg(const tN2kMsg &msg) {
    g_n2kRxCount++;
    // Health criterion H5 in its ANY_FRAME form: at least one N2K message
    // since boot (N2K-UPDATE.md 10.2).
    OtaGuard::g_anyN2kFrame = true;
    switch (msg.PGN) {
        // Proprietary addressed PGN 126720. FE 9F (manufacturer 2046) is the
        // update protocol; the receiver checks everything else itself -
        // destination, type, version, length, session. Other manufacturers'
        // 126720 (the stereo's 419 = A3 99 among them) are commands TO a
        // device; this display only sends those and has nothing to receive.
        case 126720:
            if (msg.DataLen >= 3 && msg.Data[0] == n2kupd::HDR0 && msg.Data[1] == n2kupd::HDR1)
                N2kUpdateNode::onMessage(msg);
            break;
        case 127250: onHeading(msg);      break;
        case 127245: onRudder(msg);       break;
        case 127488: onEngineRapid(msg);  break;
        case 127489: onEngineDynamic(msg);break;
        case 127508: onBattery(msg);      break;
        case 128259: onSpeed(msg);        break;
        case 128267: onDepth(msg);        break;
        case 129025: onPositionRapid(msg);break;
        case 129026: onCogSog(msg);       break;
        case 129029: onGnss(msg);         break;
        case 130306: onWind(msg);         break;
        case 129038: onAisClassA(msg);    break;
        case 129039: onAisClassB(msg);    break;
        case 129794: onAisStaticA(msg);   break;
        case 129809:
        case 129810: onAisStaticB(msg);   break;
        case 127237: onHeadingTrack(msg); break;
        case 127257: onAttitude(msg);     break;   // attitude: yaw/pitch/roll
        case 127251: onRateOfTurn(msg);   break;   // rate of turn
        case 127252: onHeave(msg);        break;   // heave (vertical motion)
        case 127505: onFluidLevel(msg);   break;   // tank fluid level
        case 127506: onDcStatus(msg);     break;   // battery DC detailed (SOC/time)
        case 130310: onOutsideEnv(msg);   break;   // water/air temp + pressure
        case 130311: onEnvParams(msg);    break;   // temp/humidity/pressure
        case 130314: onPressure(msg);     break;   // barometric pressure
        case 126992: onSystemTime(msg);   break;   // UTC system date/time
        case 129033: onLocalOffset(msg);  break;   // local time offset
        case 129283: onXte(msg);          break;   // cross-track error
        case 129284: onNavInfo(msg);      break;   // navigation data (waypoint)
        case 130320: onTideStation(msg);  break;   // tide station data
        case 127258: onMagVariation(msg); break;   // magnetic variation
        case 128275: onDistanceLog(msg);  break;   // distance log (total + trip)
        case 130312: onTempExt(msg);      break;   // temperature (extended)
        case 130820: Media::handlePGN130820(msg); break;   // stereo status (mfg 419)
        case ANCHOR_SYNC_PGN: AnchorSyncNode::onMessage(msg); break;   // anchor state
        // Pilot mode (ApPipeline). The library hands both kinds over without
        // any registration: 60928 is a system message, and RunMessageHandlers()
        // runs for those too, after the library's own claim handling and in
        // listen-only mode as well; 65305/65340/65341 sit in the proprietary
        // single-frame range 65280..65535, which CheckKnownMessage() does not
        // know - with ForwardMode 0 (HandleOnlyKnownMessages false) unknown
        // PGNs still reach this handler, and outside 126720/130816..131071
        // they are never treated as fast packets. Read only: nothing is sent.
        case ApDecode::PGN_ADDRESS_CLAIM:
            ApPipeline::addressClaim(msg.Source, msg.Data, (uint8_t)msg.DataLen);
            break;
        case ApDecode::PGN_MODE_REPORT:
        case ApDecode::PGN_STATE:
        case ApDecode::PGN_SETPOINT:      onPilotFrame(msg); break;
        default: break;
    }
}

// ---- PGN handlers -----------------------------------------------------------

void N2kHandler::onHeading(const tN2kMsg &msg) {
    if (!srcGate(N2K_SRC_HDG, msg, appConfig.cfg.srcHdg)) return;
    unsigned char SID;
    tN2kHeadingReference ref;
    double hdg, deviation, variation;
    if (!ParseN2kHeading(msg, SID, hdg, deviation, variation, ref)) return;
    auto lk = data.lock();
    if (!N2kIsNA(hdg)) {
        data.hdg = calNorm360((float)RadToDeg(hdg) + appConfig.cfg.calHdgOffsetDeg);
        // Remembered for the true wind direction only (WindCalc::Pipeline::
        // trueHeading adds deviation and variation to a magnetic heading).
        // Every other reader keeps using hdg as it always has. "Error"/
        // "unavailable" count as true, which is what the firmware assumed
        // until now. The deviation belongs to THIS reading, so a frame
        // without one clears it rather than inheriting the last frame's.
        data.hdgMagnetic   = (ref == N2khr_magnetic);
        data.hdgDeviation  = N2kIsNA(deviation) ? NAN : (float)RadToDeg(deviation);
        data.lastHdgUpdate = data.lastHdgRx = millis();
    }
    if (!N2kIsNA(variation)) {
        data.variation = (float)RadToDeg(variation);
        data.lastVariationUpdate = millis();
    }
}

void N2kHandler::onRudder(const tN2kMsg &msg) {
    unsigned char instance;
    tN2kRudderDirectionOrder dir;
    double angle, angleOrder;
    if (!ParseN2kRudder(msg, angle, instance, dir, angleOrder)) return;
    auto lk = data.lock();
    if (!N2kIsNA(angle)) {
        // Zero-point then optional sense inversion (sensor mounted mirrored).
        float a = (float)RadToDeg(angle) - appConfig.cfg.calRudderZeroDeg;
        data.rudderAngle = appConfig.cfg.calRudderInvert ? -a : a;
        data.lastRudderUpdate = millis();
    }
}

// Attitude / motion from an attitude sensor (AHRS). Angles arrive in radians.
void N2kHandler::onAttitude(const tN2kMsg &msg) {       // PGN 127257
    if (!srcGate(N2K_SRC_ATT, msg, appConfig.cfg.srcAtt)) return;
    unsigned char SID;
    double yaw, pitch, roll;
    if (!ParseN2kAttitude(msg, SID, yaw, pitch, roll)) return;
    auto lk = data.lock();
    if (!N2kIsNA(yaw))   data.yaw   = fmodf((float)RadToDeg(yaw) + 360.f, 360.f);
    if (!N2kIsNA(pitch)) data.pitch = (float)RadToDeg(pitch) - appConfig.cfg.calPitchZeroDeg;
    if (!N2kIsNA(roll))  data.roll  = (float)RadToDeg(roll)  - appConfig.cfg.calRollZeroDeg;
    // Only a frame that carried something: an all-"not available" one (the
    // attitude of an AHRS that has dropped out) must let the old angles age.
    // Yaw on its own: it often comes from another source than heel and trim.
    if (!N2kIsNA(pitch) || !N2kIsNA(roll)) data.lastAttitudeUpdate = millis();
    if (!N2kIsNA(yaw)) data.lastYawUpdate = millis();
}

void N2kHandler::onRateOfTurn(const tN2kMsg &msg) {     // PGN 127251
    unsigned char SID;
    double rot;
    if (!ParseN2kRateOfTurn(msg, SID, rot)) return;
    auto lk = data.lock();
    if (!N2kIsNA(rot)) {
        data.rateOfTurn = (float)RadToDeg(rot) * 60.0f;   // rad/s -> deg/min
        data.lastRotUpdate = millis();
    }
}

void N2kHandler::onHeave(const tN2kMsg &msg) {          // PGN 127252
    unsigned char SID;
    double heave;
    if (!ParseN2kHeave(msg, SID, heave)) return;
    auto lk = data.lock();
    if (!N2kIsNA(heave)) {
        data.heave = (float)heave;
        data.lastHeaveUpdate = millis();
        data.pushHeaveSample((float)heave, millis());   // lock held; helper is lock-free
    }
}

void N2kHandler::onFluidLevel(const tN2kMsg &msg) {     // PGN 127505
    unsigned char instance;
    tN2kFluidType ft;
    double level, capacity;
    if (!ParseN2kFluidLevel(msg, instance, ft, level, capacity)) return;
    if (ft == N2kft_Error || ft == N2kft_Unavailable) return;
    auto lk = data.lock();
    TankInfo *t = data.findOrCreateTank(instance, (uint8_t)ft);
    if (!t) return;
    if (!N2kIsNA(level))    { t->level = (float)level; t->lastUpdate = millis(); }
    if (!N2kIsNA(capacity)) t->capacity = (float)capacity;
}

void N2kHandler::onOutsideEnv(const tN2kMsg &msg) {     // PGN 130310
    if (!srcGate(N2K_SRC_ENV, msg, appConfig.cfg.srcEnv)) return;
    unsigned char SID;
    double waterT, airT, press;
    if (!ParseN2kOutsideEnvironmentalParameters(msg, SID, waterT, airT, press)) return;
    auto lk = data.lock();
    const uint32_t now = millis();
    if (!N2kIsNA(waterT)) { data.waterTemp = (float)(waterT - 273.15) + appConfig.cfg.calWaterTempOffC;
                            data.lastWaterTempUpdate = now; }
    if (!N2kIsNA(airT))   { data.airTemp   = (float)(airT   - 273.15);
                            data.lastAirTempUpdate = now; }
    if (!N2kIsNA(press))  { data.pressure = (float)(press / 100.0) + appConfig.cfg.calPressureOffHpa;
                            data.lastPressureUpdate = now;
                            data.pushPressureSample(data.pressure, now); }
}

void N2kHandler::onEnvParams(const tN2kMsg &msg) {     // PGN 130311
    if (!srcGate(N2K_SRC_ENV, msg, appConfig.cfg.srcEnv)) return;
    unsigned char SID;
    tN2kTempSource     ts;
    tN2kHumiditySource hs;
    double temp, humid, press;
    if (!ParseN2kEnvironmentalParameters(msg, SID, ts, temp, hs, humid, press)) return;
    auto lk = data.lock();
    const uint32_t now = millis();
    if (!N2kIsNA(temp)) {
        float c = (float)(temp - 273.15);
        if (ts == N2kts_SeaTemperature) { data.waterTemp = c + appConfig.cfg.calWaterTempOffC;
                                          data.lastWaterTempUpdate = now; }
        else                            { data.airTemp = c; data.lastAirTempUpdate = now; }
    }
    if (!N2kIsNA(humid)) { data.humidity = (float)humid; data.lastHumidityUpdate = now; }
    if (!N2kIsNA(press)) { data.pressure = (float)(press / 100.0) + appConfig.cfg.calPressureOffHpa;
                           data.lastPressureUpdate = now;
                           data.pushPressureSample(data.pressure, now); }
}

void N2kHandler::onPressure(const tN2kMsg &msg) {       // PGN 130314
    unsigned char SID, inst;
    tN2kPressureSource ps;
    double press;
    if (!ParseN2kPressure(msg, SID, inst, ps, press)) return;
    if (ps != N2kps_Atmospheric) return;               // only barometric
    if (!srcGate(N2K_SRC_ENV, msg, appConfig.cfg.srcEnv)) return;
    auto lk = data.lock();
    if (!N2kIsNA(press)) { data.pressure = (float)(press / 100.0) + appConfig.cfg.calPressureOffHpa;
                           data.lastPressureUpdate = millis();
                           data.pushPressureSample(data.pressure, millis()); }
}

void N2kHandler::onSystemTime(const tN2kMsg &msg) {    // PGN 126992
    unsigned char SID;
    uint16_t date;
    double secs;
    tN2kTimeSource src;
    if (!ParseN2kSystemTime(msg, SID, date, secs, src)) return;
    if (date == 0xFFFF || N2kIsNA(secs)) return;
    auto lk = data.lock();
    data.sysDays = date;
    data.sysSecOfDay = secs;
    data.lastTimeUpdate = millis();
    data.timeValid = true;
}

void N2kHandler::onLocalOffset(const tN2kMsg &msg) {  // PGN 129033
    uint16_t date;
    double secs;
    int16_t off;
    if (!ParseN2kLocalOffset(msg, date, secs, off)) return;
    auto lk = data.lock();
    if (date != 0xFFFF && !N2kIsNA(secs)) {
        data.sysDays = date;
        data.sysSecOfDay = secs;
        data.lastTimeUpdate = millis();
        data.timeValid = true;
    }
    if (off != 0x7FFF) {
        data.localOffsetMin    = off;
        data.localOffsetIsRule = false;   // the bus knows the zone; SNTP's rule stops
    }
}

void N2kHandler::onXte(const tN2kMsg &msg) {           // PGN 129283
    unsigned char SID;
    tN2kXTEMode mode;
    bool navTerm;
    double xte;
    if (!ParseN2kXTE(msg, SID, mode, navTerm, xte)) return;
    auto lk = data.lock();
    if (!N2kIsNA(xte)) { data.navXte = (float)xte; data.lastXteUpdate = millis(); }   // metres
}

void N2kHandler::onNavInfo(const tN2kMsg &msg) {      // PGN 129284
    unsigned char SID;
    double dtw, etaTime, brgOrig, brgPos, destLat, destLon, vmc;
    tN2kHeadingReference brgRef;
    tN2kDistanceCalculationType calcType;
    bool perpCrossed, arrived;
    int16_t etaDate;
    uint32_t originWp, destWp;
    if (!ParseN2kNavigationInfo(msg, SID, dtw, brgRef, perpCrossed, arrived, calcType,
            etaTime, etaDate, brgOrig, brgPos, originWp, destWp, destLat, destLon, vmc)) return;
    auto lk = data.lock();
    // Each value belongs to this frame: one marked "not available" shows
    // "--" at once instead of the last one (only this handler and the demo
    // write them). A frame with none at all - a plotter that lost its fix,
    // or one still sending after the route ended - is not a route.
    data.navDtw = N2kIsNA(dtw)    ? NAN : (float)(dtw / 1852.0);     // m → nm
    data.navBtw = N2kIsNA(brgPos) ? NAN : (float)RadToDeg(brgPos);
    data.navVmc = N2kIsNA(vmc)    ? NAN : (float)(vmc / 0.5144);     // m/s → kn
    if (destWp != 0xFFFFFFFF) data.navWpNum = destWp;
    if (!N2kIsNA(dtw) || !N2kIsNA(brgPos) || !N2kIsNA(vmc)) {
        data.navActive = true;
        data.lastNavUpdate = millis();
    }
}

void N2kHandler::onMagVariation(const tN2kMsg &msg) {   // PGN 127258
    unsigned char SID; tN2kMagneticVariation src; uint16_t days; double var;
    if (!ParseN2kMagneticVariation(msg, SID, src, days, var)) return;
    if (N2kIsNA(var)) return;
    auto lk = data.lock();
    data.variation = (float)RadToDeg(var);
    data.lastVariationUpdate = millis();
}

void N2kHandler::onDistanceLog(const tN2kMsg &msg) {   // PGN 128275
    uint16_t days; double sec; uint32_t log, trip;
    if (!ParseN2kDistanceLog(msg, days, sec, log, trip)) return;
    auto lk = data.lock();
    if (log  != N2kUInt32NA) data.logDistance  = (float)(log  / 1852.0);   // m → nm
    if (trip != N2kUInt32NA) data.tripDistance = (float)(trip / 1852.0);
    if (log != N2kUInt32NA || trip != N2kUInt32NA) data.lastLogUpdate = millis();
}

void N2kHandler::onTempExt(const tN2kMsg &msg) {       // PGN 130312
    if (!srcGate(N2K_SRC_ENV, msg, appConfig.cfg.srcEnv)) return;
    unsigned char SID, inst; tN2kTempSource src; double actual, setT;
    if (!ParseN2kTemperature(msg, SID, inst, src, actual, setT)) return;
    if (N2kIsNA(actual)) return;
    float c = (float)(actual - 273.15);                 // K → °C
    auto lk = data.lock();
    if      (src == N2kts_SeaTemperature)     { data.waterTemp = c + appConfig.cfg.calWaterTempOffC;
                                                data.lastWaterTempUpdate = millis(); }
    else if (src == N2kts_OutsideTemperature) { data.airTemp   = c; data.lastAirTempUpdate = millis(); }
}

// PGN 130320 Tide Station Data. The NMEA2000 library has no parser for this
// PGN, so we decode the fields by hand (little-endian, sequential):
//   byte 0     : Mode (4b) | Tide Tendency (2b) | reserved (2b)
//   bytes 1-2  : Measurement Date  (days since 1970)
//   bytes 3-6  : Measurement Time  (0.0001 s)
//   bytes 7-10 : Station Latitude  (1e-7 deg)
//   bytes 11-14: Station Longitude (1e-7 deg)
//   bytes 15-16: Tide Level        (0.001 m, signed)
//   bytes 17-18: Tide Level std dev (0.01 m)
//   then       : Station ID + Station Name (STRINGLAU, variable)
void N2kHandler::onTideStation(const tN2kMsg &msg) {       // PGN 130320
    int Index = 0;
    unsigned char b0 = msg.GetByte(Index);
    uint8_t tendency  = (b0 >> 4) & 0x03;          // 0 = falling, 1 = rising
    msg.Get2ByteUInt(Index);                       // measurement date (skip)
    msg.Get4ByteUDouble(0.0001, Index);            // measurement time (skip)
    msg.Get4ByteDouble(1e-7, Index);               // station lat (skip)
    msg.Get4ByteDouble(1e-7, Index);               // station lon (skip)
    double level = msg.Get2ByteDouble(0.001, Index);   // tide level [m]
    msg.Get2ByteUDouble(0.01, Index);              // std dev (skip)
    char id[24]   = {0}; size_t idLen   = sizeof(id);
    char name[24] = {0}; size_t nameLen = sizeof(name);
    msg.GetVarStr(idLen, id, Index);               // station ID  (STRINGLAU)
    msg.GetVarStr(nameLen, name, Index);           // station name(STRINGLAU)

    auto lk = data.lock();
    if (!N2kIsNA(level)) data.tideBusLevel = (float)level;
    data.tideBusRising = (tendency == 1);
    const char *st = (name[0] ? name : (id[0] ? id : "NMEA2000"));
    strncpy(data.tideBusStation, st, sizeof(data.tideBusStation) - 1);
    data.tideBusStation[sizeof(data.tideBusStation) - 1] = 0;
    data.lastTideBusUpdate = millis();
}

void N2kHandler::onEngineRapid(const tN2kMsg &msg) {
    unsigned char instance;
    double rpm, boost;
    int8_t trimPos;
    if (!ParseN2kEngineParamRapid(msg, instance, rpm, boost, trimPos)) return;
    auto lk = data.lock();
    data.engineInstance = instance;
    if (!N2kIsNA(rpm)) {
        data.rpm = (float)rpm;
        data.lastEngineUpdate = millis();
    }
}

void N2kHandler::onEngineDynamic(const tN2kMsg &msg) {
    unsigned char instance;
    double oilPress, oilTemp, coolantTemp, altVolt, fuelRate, hours;
    if (!ParseN2kEngineDynamicParam(msg, instance, oilPress, oilTemp,
            coolantTemp, altVolt, fuelRate, hours)) return;
    auto lk = data.lock();
    const uint32_t now = millis();
    if (!N2kIsNA(oilPress))    { data.oilPressure = (float)(oilPress / 100.0);       // Pa→hPa
                                 data.lastOilUpdate = now; }
    if (!N2kIsNA(coolantTemp)) { data.coolantTemp = (float)(coolantTemp - 273.15f);
                                 data.lastCoolantUpdate = now; }
    // Per the library docs (N2kMessages.h:1148), ParseN2kEngineDynamicParam
    // delivers FuelRate ALREADY in litres per hour. The earlier multiplication by
    // 3600 (comment: "m³/s→L/h") was therefore wrong and made the value 3600 times
    // too large — 4 L/h was displayed as 14400 L/h. It never showed up because the
    // demo data sets fuelFlow directly in L/h and only the real bus is affected.
    if (!N2kIsNA(fuelRate))    { data.fuelFlow = (float)fuelRate;                   // L/h
                                 data.lastFuelFlowUpdate = now; }
    // EngineHours arrives in SECONDS (N2kMessages.h:1149), but is displayed in
    // hours ("h"). Without this conversion an engine with 1287 h showed the value
    // 4633200.
    if (!N2kIsNA(hours))       { data.engineHours = (float)(hours / 3600.0);        // s→h
                                 data.lastEngineHoursUpdate = now; }
}

void N2kHandler::onBattery(const tN2kMsg &msg) {
    unsigned char instance;
    double voltage, current, temperature;
    unsigned char SID;
    if (!ParseN2kDCBatStatus(msg, instance, voltage, current, temperature, SID)) return;
    auto lk = data.lock();
    BatteryBank *b = data.findOrCreateBattery(instance);
    if (b) {
        if (!N2kIsNA(voltage))     b->voltage     = (float)voltage;
        if (!N2kIsNA(current))     b->current     = (float)current;       // + = charging
        if (!N2kIsNA(temperature)) b->temperature = (float)(temperature - 273.15f);
        if (!N2kIsNA(voltage) || !N2kIsNA(current)) b->lastUpdate = millis();
    }
    if (instance == 0) {  // keep legacy single-bank fields (grid "battv", etc.)
        if (!N2kIsNA(voltage)) data.batteryVoltage = (float)voltage;
        if (!N2kIsNA(current)) data.batteryCurrent = (float)current;
        if (!N2kIsNA(voltage) || !N2kIsNA(current)) data.lastBattUpdate = millis();
    }
}

void N2kHandler::onDcStatus(const tN2kMsg &msg) {       // PGN 127506
    unsigned char SID, inst, soc, soh;
    tN2kDCType dcType;
    double timeRem, ripple, capacity;
    if (!ParseN2kDCStatus(msg, SID, inst, dcType, soc, soh, timeRem, ripple, capacity)) return;
    auto lk = data.lock();
    BatteryBank *b = data.findOrCreateBattery(inst);
    if (!b) return;
    if (soc != 0xFF && soc <= 100) b->soc = (float)soc;             // 0xFF = N/A
    if (!N2kIsNA(timeRem))         b->timeRemMin = (float)(timeRem / 60.0);  // s → min
    // Its own stamp: with one for both PGNs a live 127506 kept a dead
    // 127508's voltage on screen, and the other way round.
    if ((soc != 0xFF && soc <= 100) || !N2kIsNA(timeRem)) b->lastStatusUpdate = millis();
}

void N2kHandler::onSpeed(const tN2kMsg &msg) {
    if (!srcGate(N2K_SRC_STW, msg, appConfig.cfg.srcStw)) return;
    unsigned char SID;
    double waterRef, groundRef;
    tN2kSpeedWaterReferenceType type;
    if (!ParseN2kBoatSpeed(msg, SID, waterRef, groundRef, type)) return;
    auto lk = data.lock();
    if (!N2kIsNA(waterRef)) {
        data.stw = (float)(waterRef / 0.5144f)                    // m/s → kn
                   * (appConfig.cfg.calStwFactorPct / 100.0f);    // paddle-wheel cal
        data.lastStwUpdate = data.lastStwRx = millis();           // true wind: fresh?
    }
}

void N2kHandler::onDepth(const tN2kMsg &msg) {
    if (!srcGate(N2K_SRC_DEPTH, msg, appConfig.cfg.srcDepth)) return;
    unsigned char SID;
    double depth, offset, range;
    if (!ParseN2kWaterDepth(msg, SID, depth, offset, range)) return;
    auto lk = data.lock();
    if (!N2kIsNA(depth)) {
        // Additive transducer offset (waterline vs keel); never below zero.
        float d = (float)depth + appConfig.cfg.calDepthOffsetM;
        data.depth = d < 0.f ? 0.f : d;
        if (!N2kIsNA(offset)) data.depthOffset = (float)offset;
        data.pushDepthSample(data.depth);
        data.lastDepthUpdate = millis();
    }
}

void N2kHandler::onPositionRapid(const tN2kMsg &msg) {
    if (!srcGate(N2K_SRC_POS, msg, appConfig.cfg.srcPos)) return;
    double lat, lon;
    if (!ParseN2kPGN129025(msg, lat, lon)) return;
    auto lk = data.lock();
    // Stamped only with a position: a GPS that has lost its fix but keeps
    // sending "not available" used to keep the LAST fix looking current - to
    // the anchor watch as well.
    if (N2kIsNA(lat) || N2kIsNA(lon)) return;
    data.lat = (float)lat;
    data.lon = (float)lon;
    data.lastKnownLat = data.lat;
    data.lastKnownLon = data.lon;
    data.lastGpsUpdate = millis();
}

void N2kHandler::onCogSog(const tN2kMsg &msg) {
    if (!srcGate(N2K_SRC_POS, msg, appConfig.cfg.srcPos)) return;
    unsigned char SID;
    tN2kHeadingReference ref;
    double cog, sog;
    if (!ParseN2kCOGSOGRapid(msg, SID, ref, cog, sog)) return;
    auto lk = data.lock();
    if (!N2kIsNA(cog)) {
        data.cog = (float)RadToDeg(cog);
        // For TWD without a compass (cal.twd_cog) only. "Error"/"unavailable"
        // count as true, as for the heading.
        data.cogMagnetic   = (ref == N2khr_magnetic);
        data.lastCogUpdate = millis();
    } else {
        // "Not available" (no fix, or a GPS that drops COG when stopped):
        // the true wind must not run on the last course for another 5 s.
        // data.cog keeps it for the other screens, as before.
        data.lastCogUpdate = 0;
    }
    // The course stays shown while 129026 keeps coming with a SOG, i.e. at
    // anchor (see DataModel.h, lastCogRx); it goes blank when the GPS goes
    // quiet or has no fix and marks both "not available".
    if (!N2kIsNA(cog) || !N2kIsNA(sog)) data.lastCogRx = millis();
    if (!N2kIsNA(sog)) {
        data.sog = (float)(sog / 0.5144f);                 // m/s → kn
        data.lastSogUpdate = data.lastSogRx = millis();    // true wind without a log
    }
}

void N2kHandler::onGnss(const tN2kMsg &msg) {
    // Full GNSS position (PGN 129029) – delivers lat/lon.
    //
    // Two things were wrong here:
    //  * The enum outputs were cast as `(tN2kGNSStype&)GNSStype` out of an
    //    `unsigned char`. The enum is 4 bytes, the variable 1 —
    //    so the parser wrote 4 bytes into a 1-byte slot and thus over the
    //    neighbours on the stack. Undefined behaviour; it only went unnoticed
    //    because nobody read the affected variables afterwards.
    //  * Per the library, the 17th parameter is **AgeOfCorrection** (age of the
    //    DGNSS correction, 0.01 s per bit) — not the magnetic variation. We
    //    converted it with RadToDeg and wrote it to data.variation, thereby
    //    overwriting the CORRECT value from PGN 127258. Visible only on boats with
    //    DGNSS/RTK, because otherwise the library delivers "not available".
    if (!srcGate(N2K_SRC_POS, msg, appConfig.cfg.srcPos)) return;
    unsigned char  SID, nSatellites, refStations;
    uint16_t       daysSince1970, refStationId;
    double         secondsSinceMidnight, lat, lon, alt;
    double         hdop, pdop, sep, ageOfCorrection;
    tN2kGNSStype   gnssType, refStationType;
    tN2kGNSSmethod gnssMethod;
    if (!ParseN2kGNSS(msg, SID, daysSince1970, secondsSinceMidnight,
            lat, lon, alt, gnssType, gnssMethod,
            nSatellites, hdop, pdop, sep,
            refStations, refStationType, refStationId,
            ageOfCorrection)) return;
    // "No GNSS" is a receiver without a fix telling us so: whatever position
    // it still fills in is not current. Like 129025, only a real position is
    // taken and stamped.
    if (gnssMethod == N2kGNSSm_noGNSS || N2kIsNA(lat) || N2kIsNA(lon)) return;
    auto lk = data.lock();
    data.lat = (float)lat;
    data.lon = (float)lon;
    data.lastKnownLat = data.lat;
    data.lastKnownLon = data.lon;
    // Magnetic variation comes from PGN 127258 (onMagVariation) — deliberately nothing here.
    data.lastGpsUpdate = millis();
}

void N2kHandler::onWind(const tN2kMsg &msg) {
    if (!srcGate(N2K_SRC_WIND, msg, appConfig.cfg.srcWind)) return;
    unsigned char SID;
    double speed, angle;
    tN2kWindReference ref;
    if (!ParseN2kWindSpeed(msg, SID, speed, angle, ref)) return;
    // A field the sender marks "not available" becomes NaN here. Converted
    // as it was, the library's N/A marker read as minus two billion knots.
    const float spKn   = N2kIsNA(speed) ? NAN : (float)(speed / 0.5144f);   // m/s → kn
    const float angDeg = N2kIsNA(angle) ? NAN : calNorm180((float)RadToDeg(angle));
    auto lk = data.lock();
    const uint32_t now = millis();   // read under the lock, after every stamp it is compared with
    switch (ref) {
        case N2kWind_Apparent:
            // The one thing the boat measures. Vane rotation and anemometer
            // scale apply here, to the raw apparent wind, so the true wind the
            // display computes from it carries the correction too.
            WindPipeline::apparent(
                spKn * (appConfig.cfg.calAwsFactorPct / 100.0f),
                calNorm180(angDeg + appConfig.cfg.calAwaOffsetDeg), now);
            break;
        // True wind calculated by ANOTHER device: 4 = over water (heading/STW),
        // 3 = boat referenced over ground (COG/SOG), 0 = north referenced
        // (TWD). Only a fallback for a bus without an apparent wind of its
        // own, and then the best reference still talking wins - see
        // WindCalc::Pipeline. These used to be written straight into tws/twa/
        // twd, last frame wins, next to an apparent wind that had nothing to
        // do with them; a simulator sending two unrelated sine curves as AWS
        // and TWS showed up exactly like that on the display. The device's own
        // calibration does not apply to them: the sender used its own.
        case N2kWind_True_water:
        case N2kWind_True_boat:
        case N2kWind_True_North:
            WindPipeline::receivedTrue((uint8_t)ref, spKn, angDeg, now);
            break;
        default: break;   // 1 = magnetic (ground referenced): not used
    }
}

// ---- AIS --------------------------------------------------------------------

// AIS strings are padded to their full length; what is left after the
// library has turned the '@' padding into NULs is trailing blanks.
static void aisCopyStr(char *dst, size_t dstSize, const char *src) {
    strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = 0;
    for (size_t n = strlen(dst); n > 0 && dst[n - 1] == ' '; n--) dst[n - 1] = 0;
}

static void fillAisPositionA(const tN2kMsg &msg) {
    uint8_t                      messageID, sid, seconds;
    tN2kAISRepeat                repeat;
    tN2kAISNavStatus             navStatus;
    tN2kAISTransceiverInformation aisInfo;
    uint32_t userID;
    double   lat, lon, cog, heading, sog, rateOfTurn;
    bool     posAcc, raim;
    // Seconds, COG, SOG - in this order (N2kMessages.h). The call used to pass
    // a "maneuver" byte and then sog, cog: every class A target came in with
    // its course in radians as its speed and its speed as its course, so its
    // vector pointed nowhere and its CPA/TCPA were wrong. Class B (129039)
    // always had the right order. Two doubles side by side - the compiler
    // cannot catch this, so keep the names as the library spells them.
    if (!ParseN2kPGN129038(msg, messageID, repeat, userID, lat, lon,
            posAcc, raim, seconds, cog, sog,
            heading, rateOfTurn, navStatus, aisInfo, sid)) return;
    // A report without a position is none: it renewed lastSeen, so a vessel that had
    // lost its GPS stayed on the radar at its old position for as long as it
    // transmitted. Now it goes 5 minutes after its last position.
    if (N2kIsNA(lat) || N2kIsNA(lon)) return;
    auto lk = data.lock();
    AisTarget *t = data.findOrCreateAis(userID, (float)lat, (float)lon,
                                        N2kIsNA(sog) ? NAN : (float)(sog / 0.5144f),
                                        N2kIsNA(cog) ? NAN : (float)RadToDeg(cog),
                                        2.f * appConfig.cfg.aisTcpaAlarm);
    t->lat = (float)lat;
    t->lon = (float)lon;
    if (!N2kIsNA(sog)) t->sog = (float)(sog / 0.5144f);
    if (!N2kIsNA(cog)) t->cog = (float)RadToDeg(cog);
    if (!N2kIsNA(heading)) t->hdg = (float)RadToDeg(heading);
    // rad/s → deg/min, same as for own ship (onRateOfTurn). Previously the raw
    // value ended up in a field that per DataModel.h carries deg/min — a factor of
    // 3437.75 too small; a target turning at 30 deg/min was stored as 0.0087.
    if (!N2kIsNA(rateOfTurn)) t->rateOfTurn = (float)(RadToDeg(rateOfTurn) * 60.0);
    t->navStatus = navStatus;
    t->classB    = false;
    t->lastSeen  = data.lastAisRx = millis();
    data.calcCpa(*t);
}

void N2kHandler::onAisClassA(const tN2kMsg &msg) { fillAisPositionA(msg); }

void N2kHandler::onAisClassB(const tN2kMsg &msg) {
    uint8_t       messageID, seconds, sid;
    tN2kAISRepeat repeat;
    uint32_t      userID;
    double        lat, lon, cog, hdg, sog;
    bool          posAcc, raim;
    tN2kAISTransceiverInformation aisInfo;
    tN2kAISUnit   unit;
    tN2kAISMode   mode;
    bool          display, dsc, band, msg22, state;
    if (!ParseN2kPGN129039(msg, messageID, repeat, userID, lat, lon,
            posAcc, raim, seconds, cog, sog, aisInfo, hdg, unit,
            display, dsc, band, msg22, mode, state, sid)) return;
    if (N2kIsNA(lat) || N2kIsNA(lon)) return;   // see fillAisPositionA()
    auto lk = data.lock();
    AisTarget *t = data.findOrCreateAis(userID, (float)lat, (float)lon,
                                        N2kIsNA(sog) ? NAN : (float)(sog / 0.5144f),
                                        N2kIsNA(cog) ? NAN : (float)RadToDeg(cog),
                                        2.f * appConfig.cfg.aisTcpaAlarm);
    t->lat = (float)lat;
    t->lon = (float)lon;
    if (!N2kIsNA(sog)) t->sog = (float)(sog / 0.5144f);
    if (!N2kIsNA(cog)) t->cog = (float)RadToDeg(cog);
    if (!N2kIsNA(hdg)) t->hdg = (float)RadToDeg(hdg);
    t->classB   = true;
    t->lastSeen = data.lastAisRx = millis();
    data.calcCpa(*t);
}

void N2kHandler::onAisStaticA(const tN2kMsg &msg) {
    uint8_t        messageID, shipType, SID;
    tN2kAISRepeat  repeat;
    uint32_t       userID, IMOnumber;
    char           callsign[8], name[21], destination[21];
    double         length, beam, posRefStbd, posRefBow;
    uint16_t       etaDate;
    double         etaTime, draught;
    tN2kAISVersion aisVersion;
    tN2kGNSStype   gnssType;
    tN2kAISDTE     dte;
    tN2kAISTransceiverInformation aisInfo;
    if (!ParseN2kPGN129794(msg, messageID, repeat, userID,
            IMOnumber, callsign, sizeof(callsign), name, sizeof(name),
            shipType, length, beam, posRefStbd, posRefBow,
            etaDate, etaTime, draught,
            destination, sizeof(destination),
            aisVersion, gnssType, dte, aisInfo, SID)) return;
    auto lk = data.lock();
    AisTarget *t = data.findOrCreateAis(userID);
    strncpy(t->name, name, 20); t->name[20] = 0;
    aisCopyStr(t->callsign, sizeof(t->callsign), callsign);
    t->shipType = shipType;
}

// Class B static data comes in two halves: part A (129809) carries the name,
// part B (129810) the ship type and the callsign. Part B used to be dropped
// unread, so a class B vessel never had a callsign or a type - and one whose
// part A had not come yet had no label at all.
void N2kHandler::onAisStaticB(const tN2kMsg &msg) {
    uint8_t       messageID, sid;
    tN2kAISRepeat repeat;
    uint32_t      userID;
    tN2kAISTransceiverInformation aisInfo;
    if (msg.PGN == 129809) {
        char name[21];
        if (!ParseN2kPGN129809(msg, messageID, repeat, userID, name, sizeof(name), aisInfo, sid)) return;
        auto lk = data.lock();
        AisTarget *t = data.findOrCreateAis(userID);
        strncpy(t->name, name, 20); t->name[20] = 0;
        t->classB = true;
    } else {   // 129810
        uint8_t  vesselType;
        char     vendor[8], callsign[8];
        double   length, beam, posRefStbd, posRefBow;
        uint32_t mothershipId;
        if (!ParseN2kPGN129810(msg, messageID, repeat, userID, vesselType,
                vendor, sizeof(vendor), callsign, sizeof(callsign),
                length, beam, posRefStbd, posRefBow, mothershipId, aisInfo, sid)) return;
        auto lk = data.lock();
        AisTarget *t = data.findOrCreateAis(userID);
        aisCopyStr(t->callsign, sizeof(t->callsign), callsign);
        t->shipType = vesselType;
        t->classB   = true;
    }
}

void N2kHandler::onHeadingTrack(const tN2kMsg &msg) {
    // PGN 127237 – Heading/Track Control (autopilot)
    // Heading-to-steer and the commanded rudder only. Its steering mode is
    // deliberately ignored: pilots have been seen reporting 7 there even
    // while engaged. The mode comes from onPilotFrame().
    auto lk = data.lock();
    // Was a hand-rolled "read bytes 2-3 as the commanded heading" fallback — but
    // at that offset sit the reserved bits + commanded-rudder-direction and only
    // the LOW byte of the rudder angle, so the value was never the target course.
    // The library does have a proper parser; use it and take Heading-To-Steer.
    tN2kOnOff rudLim, offHdgLim, offTrkLim, ovr;
    tN2kSteeringMode steerMode; tN2kTurnMode turnMode;
    tN2kHeadingReference hdgRef; tN2kRudderDirectionOrder rudDir;
    double cmdRudder, cmdHeading, track, rudLimit, offHdgLimit;
    double radiusOrder, rotOrder, offTrkLimit, vesselHeading;
    if (!ParseN2kHeadingTrackControl(msg, rudLim, offHdgLim, offTrkLim, ovr,
            steerMode, turnMode, hdgRef, rudDir, cmdRudder, cmdHeading, track,
            rudLimit, offHdgLimit, radiusOrder, rotOrder, offTrkLimit, vesselHeading))
        return;
    // One stamp for both: they come in the same frame and go blank together.
    if (!N2kIsNA(cmdRudder)) { data.apRudder = (float)RadToDeg(cmdRudder); data.lastApUpdate = millis(); }
    if (!N2kIsNA(cmdHeading)) {
        data.apTargetHeading = (float)RadToDeg(cmdHeading);
        data.lastApUpdate = millis();
    }
}

// PGN 65305 / 65340 / 65341 - proprietary, manufacturer code 1857: the pilot
// computer's mode, status and setpoint. The rules are in ApDecode.h.
//
// The NAME test comes first so that the source picker only ever lists pilot
// computers, then the usual source gate (a pinned pilot computer wins when
// there are two), then the decoder - which checks header and NAME again,
// because the simulator's frame player enters at that point.
void N2kHandler::onPilotFrame(const tN2kMsg &msg) {
    if (!ApPipeline::isPilotComputer(msg.Source)) {
        // Still decoded: the pipeline logs once per address when the NAME is
        // simply not known yet - the one case worth a line in the log. It
        // changes nothing in the model for such a frame.
        auto lk = data.lock();
        ApPipeline::frame(msg.PGN, msg.Source, msg.Data, (uint8_t)msg.DataLen, millis());
        return;
    }
    if (!srcGate(N2K_SRC_AP, msg, appConfig.cfg.srcAp)) return;
    auto lk = data.lock();
    ApPipeline::frame(msg.PGN, msg.Source, msg.Data, (uint8_t)msg.DataLen, millis());
}
