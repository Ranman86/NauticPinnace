#pragma once
#include <NMEA2000.h>
#include "DataModel.h"

// ============================================================
// N2kHandler – initialise the NMEA 2000 CAN interface and
// register message handlers that populate DataModel.
// ============================================================
class N2kHandler {
public:
    // Call once from setup()
    void begin();

    // Must be called frequently from the N2K task loop
    void loop();

private:
    static void handleMsg(const tN2kMsg &msg);

    static void onHeading(const tN2kMsg &msg);         // PGN 127250
    static void onRudder(const tN2kMsg &msg);           // PGN 127245
    static void onEngineRapid(const tN2kMsg &msg);      // PGN 127488
    static void onEngineDynamic(const tN2kMsg &msg);    // PGN 127489
    static void onBattery(const tN2kMsg &msg);          // PGN 127508
    static void onSpeed(const tN2kMsg &msg);            // PGN 128259
    static void onDepth(const tN2kMsg &msg);            // PGN 128267
    static void onPositionRapid(const tN2kMsg &msg);    // PGN 129025
    static void onCogSog(const tN2kMsg &msg);           // PGN 129026
    static void onGnss(const tN2kMsg &msg);             // PGN 129029
    static void onWind(const tN2kMsg &msg);             // PGN 130306
    static void onAisClassA(const tN2kMsg &msg);        // PGN 129038
    static void onAisClassB(const tN2kMsg &msg);        // PGN 129039
    static void onAisStaticA(const tN2kMsg &msg);       // PGN 129794
    static void onAisStaticB(const tN2kMsg &msg);       // PGN 129809/129810
    static void onHeadingTrack(const tN2kMsg &msg);     // PGN 127237 (autopilot)
    static void onAttitude(const tN2kMsg &msg);         // PGN 127257 (yaw/pitch/roll)
    static void onRateOfTurn(const tN2kMsg &msg);       // PGN 127251
    static void onHeave(const tN2kMsg &msg);            // PGN 127252
    static void onFluidLevel(const tN2kMsg &msg);       // PGN 127505 (tanks)
    static void onDcStatus(const tN2kMsg &msg);         // PGN 127506 (battery SOC/time)
    static void onOutsideEnv(const tN2kMsg &msg);       // PGN 130310 (water/air temp + press)
    static void onEnvParams(const tN2kMsg &msg);        // PGN 130311 (temp/humidity/press)
    static void onPressure(const tN2kMsg &msg);         // PGN 130314 (barometric)
    static void onSystemTime(const tN2kMsg &msg);       // PGN 126992 (UTC date/time)
    static void onLocalOffset(const tN2kMsg &msg);      // PGN 129033 (local time offset)
    static void onXte(const tN2kMsg &msg);              // PGN 129283 (cross-track error)
    static void onNavInfo(const tN2kMsg &msg);          // PGN 129284 (navigation data)
    static void onTideStation(const tN2kMsg &msg);      // PGN 130320 (tide station data)
    static void onMagVariation(const tN2kMsg &msg);     // PGN 127258 (magnetic variation)
    static void onDistanceLog(const tN2kMsg &msg);      // PGN 128275 (distance log)
    static void onTempExt(const tN2kMsg &msg);          // PGN 130312 (temperature)
};

extern N2kHandler n2k;
