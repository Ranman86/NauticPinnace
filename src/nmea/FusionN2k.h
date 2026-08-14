#pragma once
#include <Arduino.h>

class tN2kMsg;   // fwd decl (defined in <N2kMsg.h>)

// ============================================================
// Fusion – NMEA 2000 media control for a Fusion marine stereo (e.g. RA-670).
//
// Control functions update the DataModel optimistically (so the UI reacts
// immediately) and, when the CAN bus is live (n2kActive), transmit the matching
// Fusion proprietary command (PGN 126720). Incoming Fusion status (PGN 130820)
// is parsed into the DataModel.
//
// NOTE: the N2K task is currently DISABLED (see main.cpp), so n2kActive stays
// false and nothing is transmitted — the screen runs on demo data. Enable the
// N2K task + set Fusion::n2kActive=true to go live. The Fusion command opcodes
// are best-effort per the canboat/SignalK reverse-engineering and should be
// verified against the actual radio (they are isolated as named constants in
// FusionN2k.cpp for easy tweaking).
// ============================================================
namespace Fusion {
    extern bool    n2kActive;       // true once the CAN bus + N2K task are running
    extern uint8_t deviceAddress;   // N2K source address of the radio (0xFF=unknown)

    // Set the transmit hook (NMEA2000.SendMsg). Called by N2kHandler::begin().
    void setSendHook(void (*fn)(const tN2kMsg &));

    // ---- control (called from the Fusion screen) ----
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
    void handlePGN130820(const tN2kMsg &msg);   // Fusion status / now-playing / volume

    // ---- demo (called from DemoData when demoMode) ----
    void demoInit();                // seed a source list, a track and volumes
    void demoTick(uint32_t nowMs);  // advance elapsed time, rotate track/source
}
