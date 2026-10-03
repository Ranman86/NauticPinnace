#pragma once
#include <Arduino.h>

class tN2kMsg;   // fwd decl (defined in <N2kMsg.h>)

// ============================================================
// Media – NMEA 2000 media control for a marine stereo that speaks the
// proprietary protocol of NMEA manufacturer code 419.
//
// No brand names, here or anywhere in the project: the module and the screen are
// named for what they do, and the protocol is identified the way the bus
// identifies it - by manufacturer code 419 and its two PGNs. That is also what a
// maintainer searches for in canboat's PGN catalogue.
//
// Control functions update the DataModel optimistically (so the UI reacts
// immediately) - but only while there is something to control: the running demo
// stereo, or a radio heard on the bus. When the CAN bus is live (n2kActive, not
// in listen-only) and the demo is not running, they also transmit the matching
// proprietary command (PGN 126720). With neither, a control changes nothing.
// Incoming status (PGN 130820, manufacturer 419) is parsed into the DataModel.
//
// The command opcodes are best-effort per the canboat/SignalK reverse-engineering
// and should be verified against the actual radio (they are isolated as named
// constants in MediaN2k.cpp for easy tweaking).
// ============================================================
namespace Media {
    extern bool    n2kActive;       // true once the CAN bus + N2K task are running
    extern uint8_t deviceAddress;   // N2K source address of the radio (0xFF=unknown)

    // Set the transmit hook (NMEA2000.SendMsg). Called by N2kHandler::begin().
    void setSendHook(void (*fn)(const tN2kMsg &));

    // ---- control (called from the media screen) ----
    void setSource(int idx);
    void cycleSource(int dir);              // +1 next / -1 previous source
    void setZoneVolume(int zone, int vol);  // zone 0..2, vol 0..100
    void setMasterVolume(int vol);          // 0..100, scales all zones proportionally
    void nudgeMaster(int delta);            // +/- convenience
    void playPause();
    void nextTrack();
    void prevTrack();
    void toggleZoneMute(int zone);  // mute/unmute one zone (remembers its volume)
    void toggleAllMute();           // master mute: mute all, or unmute all if any muted
    void requestStatus();                   // ask the radio for state + source list

    // ---- incoming (dispatched from N2kHandler::handleMsg) ----
    void handlePGN130820(const tN2kMsg &msg);   // stereo status / now-playing / volume

    // ---- demo (called from DemoData when demoMode) ----
    void demoInit();                // seed a source list, a track and volumes
    void demoTick(uint32_t nowMs);  // advance elapsed time, rotate track/source
}
