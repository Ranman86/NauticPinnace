#include "N2kHandler.h"
#include "DemoData.h"
#include "FusionN2k.h"
#include "../BoardConfig.h"
#include "../config/Config.h"
#include <N2kMessages.h>

// Use custom TX/RX pins before including NMEA2000_CAN.h
#define ESP32_CAN_TX_PIN (gpio_num_t)CAN_TX_PIN
#define ESP32_CAN_RX_PIN (gpio_num_t)CAN_RX_PIN
#include <NMEA2000_CAN.h>

// ---- Init -------------------------------------------------------------------

void N2kHandler::begin() {
    // TJA1051T/3 standby pin: drive LOW to enable the transceiver.
    // If CAN_STB_PIN == -1 the pin is hardwired to GND on the PCB – nothing to do.
#if CAN_STB_PIN >= 0
    pinMode(CAN_STB_PIN, OUTPUT);
    digitalWrite(CAN_STB_PIN, LOW);
#endif

    NMEA2000.SetProductInformation(
        "NAUTICPINNACE-001",       // manufacturer's model serial code
        100,                     // manufacturer's product code
        "NauticPinnace",        // model ID
        "1.0.0",                 // software version code
        "1.0"                    // model version
    );
    NMEA2000.SetDeviceInformation(
        1001001,                 // unique device number
        120,                     // device function: display
        120,                     // device class: display
        2046                     // manufacturer code (private)
    );
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
    NMEA2000.Open();
    Serial.printf("[n2k] mode: %s\n", listenOnly ? "LISTEN-ONLY (silent)" : "listen+node");

    if (!listenOnly) {
        // Fusion media control: route TX through this node, mark the bus live.
        // In listen-only, n2kActive stays false -> every control path in
        // FusionN2k.cpp is already gated on it and becomes a no-op.
        Fusion::setSendHook([](const tN2kMsg &m) { NMEA2000.SendMsg(m); });
        Fusion::n2kActive = true;
        Fusion::requestStatus();   // ask any radio on the bus for its state + sources
    }
}

void N2kHandler::loop() {
    if (appConfig.cfg.demoMode) {
        // Demo mode: inject synthetic data instead of reading the CAN bus.
        // demoData.tick() is internally rate-limited to ~5 Hz.
        demoData.tick();
    } else {
        NMEA2000.ParseMessages();
    }
}

// ---- Message dispatcher -----------------------------------------------------

// Counts every received N2k message – shows up in the [HB] log as n2kRx.
// Useful for checking wiring/bus operation without looking at the display.
volatile uint32_t g_n2kRxCount = 0;

void N2kHandler::handleMsg(const tN2kMsg &msg) {
    g_n2kRxCount++;
    switch (msg.PGN) {
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
        case 130820: Fusion::handlePGN130820(msg); break;   // Fusion media status
        default: break;
    }
}

// ---- PGN handlers -----------------------------------------------------------

void N2kHandler::onHeading(const tN2kMsg &msg) {
    unsigned char SID;
    tN2kHeadingReference ref;
    double hdg, deviation, variation;
    if (!ParseN2kHeading(msg, SID, hdg, deviation, variation, ref)) return;
    auto lk = data.lock();
    if (!N2kIsNA(hdg)) data.hdg = (float)RadToDeg(hdg);
    if (!N2kIsNA(variation)) data.variation = (float)RadToDeg(variation);
}

void N2kHandler::onRudder(const tN2kMsg &msg) {
    unsigned char instance;
    tN2kRudderDirectionOrder dir;
    double angle, angleOrder;
    if (!ParseN2kRudder(msg, angle, instance, dir, angleOrder)) return;
    auto lk = data.lock();
    if (!N2kIsNA(angle)) {
        data.rudderAngle = (float)RadToDeg(angle);
        data.lastRudderUpdate = millis();
    }
}

// Precision-9 (or any AHRS) attitude / motion. Angles arrive in radians.
void N2kHandler::onAttitude(const tN2kMsg &msg) {       // PGN 127257
    unsigned char SID;
    double yaw, pitch, roll;
    if (!ParseN2kAttitude(msg, SID, yaw, pitch, roll)) return;
    auto lk = data.lock();
    if (!N2kIsNA(yaw))   data.yaw   = fmodf((float)RadToDeg(yaw) + 360.f, 360.f);
    if (!N2kIsNA(pitch)) data.pitch = (float)RadToDeg(pitch);
    if (!N2kIsNA(roll))  data.roll  = (float)RadToDeg(roll);
    data.lastAttitudeUpdate = millis();
}

void N2kHandler::onRateOfTurn(const tN2kMsg &msg) {     // PGN 127251
    unsigned char SID;
    double rot;
    if (!ParseN2kRateOfTurn(msg, SID, rot)) return;
    auto lk = data.lock();
    if (!N2kIsNA(rot)) data.rateOfTurn = (float)RadToDeg(rot) * 60.0f;  // rad/s -> deg/min
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
    if (!N2kIsNA(level))    t->level    = (float)level;
    if (!N2kIsNA(capacity)) t->capacity = (float)capacity;
    t->lastUpdate = millis();
}

void N2kHandler::onOutsideEnv(const tN2kMsg &msg) {     // PGN 130310
    unsigned char SID;
    double waterT, airT, press;
    if (!ParseN2kOutsideEnvironmentalParameters(msg, SID, waterT, airT, press)) return;
    auto lk = data.lock();
    if (!N2kIsNA(waterT)) data.waterTemp = (float)(waterT - 273.15);
    if (!N2kIsNA(airT))   data.airTemp   = (float)(airT   - 273.15);
    if (!N2kIsNA(press))  { data.pressure = (float)(press / 100.0); data.pushPressureSample(data.pressure, millis()); }
    data.lastEnvUpdate = millis();
}

void N2kHandler::onEnvParams(const tN2kMsg &msg) {     // PGN 130311
    unsigned char SID;
    tN2kTempSource     ts;
    tN2kHumiditySource hs;
    double temp, humid, press;
    if (!ParseN2kEnvironmentalParameters(msg, SID, ts, temp, hs, humid, press)) return;
    auto lk = data.lock();
    if (!N2kIsNA(temp)) {
        float c = (float)(temp - 273.15);
        if (ts == N2kts_SeaTemperature) data.waterTemp = c;
        else                            data.airTemp   = c;
    }
    if (!N2kIsNA(humid)) data.humidity = (float)humid;
    if (!N2kIsNA(press)) { data.pressure = (float)(press / 100.0); data.pushPressureSample(data.pressure, millis()); }
    data.lastEnvUpdate = millis();
}

void N2kHandler::onPressure(const tN2kMsg &msg) {       // PGN 130314
    unsigned char SID, inst;
    tN2kPressureSource ps;
    double press;
    if (!ParseN2kPressure(msg, SID, inst, ps, press)) return;
    if (ps != N2kps_Atmospheric) return;               // only barometric
    auto lk = data.lock();
    if (!N2kIsNA(press)) { data.pressure = (float)(press / 100.0); data.pushPressureSample(data.pressure, millis()); }
    data.lastEnvUpdate = millis();
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
    if (off != 0x7FFF) data.localOffsetMin = off;
}

void N2kHandler::onXte(const tN2kMsg &msg) {           // PGN 129283
    unsigned char SID;
    tN2kXTEMode mode;
    bool navTerm;
    double xte;
    if (!ParseN2kXTE(msg, SID, mode, navTerm, xte)) return;
    auto lk = data.lock();
    if (!N2kIsNA(xte)) data.navXte = (float)xte;     // metres
    data.lastNavUpdate = millis();
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
    if (!N2kIsNA(dtw)) data.navDtw = (float)(dtw / 1852.0);          // m → nm
    if (!N2kIsNA(brgPos)) data.navBtw = (float)RadToDeg(brgPos);
    if (!N2kIsNA(vmc)) data.navVmc = (float)(vmc / 0.5144);          // m/s → kn
    if (destWp != 0xFFFFFFFF) data.navWpNum = destWp;
    data.navActive = true;
    data.lastNavUpdate = millis();
}

void N2kHandler::onMagVariation(const tN2kMsg &msg) {   // PGN 127258
    unsigned char SID; tN2kMagneticVariation src; uint16_t days; double var;
    if (!ParseN2kMagneticVariation(msg, SID, src, days, var)) return;
    if (N2kIsNA(var)) return;
    auto lk = data.lock();
    data.variation = (float)RadToDeg(var);
}

void N2kHandler::onDistanceLog(const tN2kMsg &msg) {   // PGN 128275
    uint16_t days; double sec; uint32_t log, trip;
    if (!ParseN2kDistanceLog(msg, days, sec, log, trip)) return;
    auto lk = data.lock();
    if (log  != N2kUInt32NA) data.logDistance  = (float)(log  / 1852.0);   // m → nm
    if (trip != N2kUInt32NA) data.tripDistance = (float)(trip / 1852.0);
    data.lastLogUpdate = millis();
}

void N2kHandler::onTempExt(const tN2kMsg &msg) {       // PGN 130312
    unsigned char SID, inst; tN2kTempSource src; double actual, setT;
    if (!ParseN2kTemperature(msg, SID, inst, src, actual, setT)) return;
    if (N2kIsNA(actual)) return;
    float c = (float)(actual - 273.15);                 // K → °C
    auto lk = data.lock();
    if      (src == N2kts_SeaTemperature)     { data.waterTemp = c; data.lastEnvUpdate = millis(); }
    else if (src == N2kts_OutsideTemperature) { data.airTemp   = c; data.lastEnvUpdate = millis(); }
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
    if (!N2kIsNA(oilPress))    data.oilPressure  = (float)(oilPress / 100.0); // Pa→hPa
    if (!N2kIsNA(coolantTemp)) data.coolantTemp  = (float)(coolantTemp - 273.15f);
    // Per the library docs (N2kMessages.h:1148), ParseN2kEngineDynamicParam
    // delivers FuelRate ALREADY in litres per hour. The earlier multiplication by
    // 3600 (comment: "m³/s→L/h") was therefore wrong and made the value 3600 times
    // too large — 4 L/h was displayed as 14400 L/h. It never showed up because the
    // demo data sets fuelFlow directly in L/h and only the real bus is affected.
    if (!N2kIsNA(fuelRate))    data.fuelFlow     = (float)fuelRate;             // L/h
    // EngineHours arrives in SECONDS (N2kMessages.h:1149), but is displayed in
    // hours ("h"). Without this conversion an engine with 1287 h showed the value
    // 4633200.
    if (!N2kIsNA(hours))       data.engineHours  = (float)(hours / 3600.0);     // s→h
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
        b->lastUpdate = millis();
    }
    if (instance == 0) {  // keep legacy single-bank fields (grid "battv", etc.)
        if (!N2kIsNA(voltage)) data.batteryVoltage = (float)voltage;
        if (!N2kIsNA(current)) data.batteryCurrent = (float)current;
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
    b->lastUpdate = millis();
}

void N2kHandler::onSpeed(const tN2kMsg &msg) {
    unsigned char SID;
    double waterRef, groundRef;
    tN2kSpeedWaterReferenceType type;
    if (!ParseN2kBoatSpeed(msg, SID, waterRef, groundRef, type)) return;
    auto lk = data.lock();
    if (!N2kIsNA(waterRef)) data.stw = (float)(waterRef / 0.5144f); // m/s → kn
}

void N2kHandler::onDepth(const tN2kMsg &msg) {
    unsigned char SID;
    double depth, offset, range;
    if (!ParseN2kWaterDepth(msg, SID, depth, offset, range)) return;
    auto lk = data.lock();
    if (!N2kIsNA(depth)) {
        data.depth = (float)depth;
        if (!N2kIsNA(offset)) data.depthOffset = (float)offset;
        data.pushDepthSample(data.depth);
        data.lastDepthUpdate = millis();
    }
}

void N2kHandler::onPositionRapid(const tN2kMsg &msg) {
    double lat, lon;
    if (!ParseN2kPGN129025(msg, lat, lon)) return;
    auto lk = data.lock();
    if (!N2kIsNA(lat)) data.lat = (float)lat;
    if (!N2kIsNA(lon)) data.lon = (float)lon;
    data.lastGpsUpdate = millis();
}

void N2kHandler::onCogSog(const tN2kMsg &msg) {
    unsigned char SID;
    tN2kHeadingReference ref;
    double cog, sog;
    if (!ParseN2kCOGSOGRapid(msg, SID, ref, cog, sog)) return;
    auto lk = data.lock();
    if (!N2kIsNA(cog)) data.cog = (float)RadToDeg(cog);
    if (!N2kIsNA(sog)) data.sog = (float)(sog / 0.5144f); // m/s → kn
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
    auto lk = data.lock();
    if (!N2kIsNA(lat)) data.lat = (float)lat;
    if (!N2kIsNA(lon)) data.lon = (float)lon;
    // Magnetic variation comes from PGN 127258 (onMagVariation) — deliberately nothing here.
    data.lastGpsUpdate = millis();
}

void N2kHandler::onWind(const tN2kMsg &msg) {
    unsigned char SID;
    double speed, angle;
    tN2kWindReference ref;
    if (!ParseN2kWindSpeed(msg, SID, speed, angle, ref)) return;
    float spKn = (float)(speed / 0.5144f);  // m/s → kn
    float angDeg = (float)RadToDeg(angle);
    // Normalise to ±180
    while (angDeg >  180) angDeg -= 360;
    while (angDeg < -180) angDeg += 360;
    auto lk = data.lock();
    switch (ref) {
        case N2kWind_Apparent:
            data.aws = spKn;
            data.awa = angDeg;
            break;
        // True wind referenced to the vessel's axis. Accept BOTH references:
        // 4 = over water (Heading/STW), 3 = over ground (COG/SOG). Reference 3
        // used to fall into the default branch and was silently discarded —
        // but devices and simulators frequently send TWA/TWS exactly that way.
        case N2kWind_True_water:
        case N2kWind_True_boat:
            data.tws = spKn;
            data.twa = angDeg;
            break;
        case N2kWind_True_North:
            data.tws = spKn;
            data.twd = fmod(angDeg + 360, 360);
            break;
        default: break;
    }
    data.lastWindUpdate = millis();
    if (!isnan(data.twd)) data.pushWindSample(data.twd, data.tws);
}

// ---- AIS --------------------------------------------------------------------

static void fillAisPositionA(const tN2kMsg &msg) {
    uint8_t                      messageID, sid, maneuver;
    tN2kAISRepeat                repeat;
    tN2kAISNavStatus             navStatus;
    tN2kAISTransceiverInformation aisInfo;
    uint32_t userID;
    double   lat, lon, cog, heading, sog, rateOfTurn;
    bool     posAcc, raim;
    if (!ParseN2kPGN129038(msg, messageID, repeat, userID, lat, lon,
            posAcc, raim, maneuver, sog, cog,
            heading, rateOfTurn, navStatus, aisInfo, sid)) return;
    auto lk = data.lock();
    AisTarget *t = data.findOrCreateAis(userID);
    if (!N2kIsNA(lat)) t->lat = (float)lat;
    if (!N2kIsNA(lon)) t->lon = (float)lon;
    if (!N2kIsNA(sog)) t->sog = (float)(sog / 0.5144f);
    if (!N2kIsNA(cog)) t->cog = (float)RadToDeg(cog);
    if (!N2kIsNA(heading)) t->hdg = (float)RadToDeg(heading);
    // rad/s → deg/min, same as for own ship (onRateOfTurn). Previously the raw
    // value ended up in a field that per DataModel.h carries deg/min — a factor of
    // 3437.75 too small; a target turning at 30 deg/min was stored as 0.0087.
    if (!N2kIsNA(rateOfTurn)) t->rateOfTurn = (float)(RadToDeg(rateOfTurn) * 60.0);
    t->navStatus = navStatus;
    t->classB    = false;
    t->lastSeen  = millis();
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
    auto lk = data.lock();
    AisTarget *t = data.findOrCreateAis(userID);
    if (!N2kIsNA(lat)) t->lat = (float)lat;
    if (!N2kIsNA(lon)) t->lon = (float)lon;
    if (!N2kIsNA(sog)) t->sog = (float)(sog / 0.5144f);
    if (!N2kIsNA(cog)) t->cog = (float)RadToDeg(cog);
    if (!N2kIsNA(hdg)) t->hdg = (float)RadToDeg(hdg);
    t->classB   = true;
    t->lastSeen = millis();
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
    strncpy(t->callsign, callsign, 7); t->callsign[7] = 0;
    t->shipType = shipType;
}

void N2kHandler::onAisStaticB(const tN2kMsg &msg) {
    // PGN 129809 – Class B name/callsign
    uint8_t       messageID, sid;
    tN2kAISRepeat repeat;
    uint32_t      userID;
    if (msg.PGN == 129809) {
        char name[21];
        tN2kAISTransceiverInformation aisInfo;
        if (!ParseN2kPGN129809(msg, messageID, repeat, userID, name, sizeof(name), aisInfo, sid)) return;
        auto lk = data.lock();
        AisTarget *t = data.findOrCreateAis(userID);
        strncpy(t->name, name, 20); t->name[20] = 0;
    }
}

void N2kHandler::onHeadingTrack(const tN2kMsg &msg) {
    // PGN 127237 – Heading/Track Control (autopilot)
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
    if (!N2kIsNA(cmdRudder)) data.apRudder = (float)RadToDeg(cmdRudder);
    if (!N2kIsNA(cmdHeading)) {
        data.apTargetHeading = (float)RadToDeg(cmdHeading);
        data.lastApUpdate = millis();
    }
}
