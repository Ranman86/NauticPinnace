#pragma once
// ============================================================================
// UpdateGate - the single owner of everything that must not overlap
// (docs/N2K-UPDATE.md 11.1, 10.4, 11.5):
//
//   - an update session over NMEA 2000 (from the dialog to the activation),
//   - a WiFi OTA upload (/api/ota),
//   - the OtaGuard verify window after an update (10.2),
//   - a pending restart: a new firmware is activated but not running yet.
//
// Every task asks it: async_tcp (/api/ota, /api/restart), the display task
// (restart requests from the settings, demo mode) and the N2K task (OFFER
// BUSY). Hence a spinlock around the few fields that are written together.
//
// What it answers:
//   canStartWifiOta()  /api/ota may begin: no session, no verify window, no
//                      pending restart (the next update slot WOULD be the slot
//                      that is set to boot next), no other upload.
//   canRestart()       a deliberate restart may happen now: no session and no
//                      verify window (a restart there rolls the update back,
//                      cause 4). A PENDING restart does not block it - that
//                      restart is exactly what activates the update.
//   demoAllowed()      demo mode may take effect: it stops the N2K task from
//                      parsing, so not during a session or verify window
//                      (5.1 rule 6).
//   claimWriter()      the one OtaWriter / WebUiWriter owner (9.2).
// ============================================================================
#include <stdint.h>

namespace UpdateGate {

enum Owner : uint8_t { OWNER_NONE = 0, OWNER_N2K = 1, OWNER_WIFI = 2 };

// ---- the writer (OtaWriter / WebUiWriter) -----------------------------------
// true if the writer was free or already belongs to `o`.
bool  claimWriter(Owner o);
void  releaseWriter(Owner o);
Owner writer();

// ---- state reported by the parts ---------------------------------------------
// The receiver's session state (table 4.7); IUpdHost::sessionStateChanged().
void    setSessionState(uint8_t state);
uint8_t sessionState();
// CONFIRMING .. APPLYING: the span in which the gate blocks the rest.
bool    n2kSessionActive();

// OtaGuard: the running image is PENDING_VERIFY. `capAtMs` is the millis() by
// which it has to be decided (for the "Restart after the update (45 s)" text).
void setVerifyWindow(bool on, uint32_t capAtMs);
bool verifyWindow();

// A firmware has been activated (boot partition set) but is not running yet.
void setRestartPending(bool on);
bool restartPending();

// ---- answers ------------------------------------------------------------------
bool wifiOtaActive();          // OFFER -> BUSY (5.8)
bool canStartWifiOta();
bool canRestart();
bool demoAllowed();
// Seconds until canRestart() is expected to turn true, 0 if not known.
uint32_t restartBlockedSeconds();
// Short name of the current owner for /api/info: "idle", "n2k", "wifi",
// "verify", "restart_pending".
const char *describe();

}  // namespace UpdateGate
