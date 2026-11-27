// ============================================================================
// UpdateGate.cpp - see UpdateGate.h.
// ============================================================================
#include "UpdateGate.h"
#include "N2kUpdateProto.h"   // n2kupd::State
#include <Arduino.h>

namespace UpdateGate {
namespace {

// The simulator has one thread that matters and no FreeRTOS port layer, so
// the guard compiles away there (the same trick as AnchorSyncNode).
#ifdef SIMULATOR
#define GATE_LOCK()   do {} while (0)
#define GATE_UNLOCK() do {} while (0)
#else
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define GATE_LOCK()   portENTER_CRITICAL(&s_mux)
#define GATE_UNLOCK() portEXIT_CRITICAL(&s_mux)
#endif

volatile uint8_t  s_writer         = OWNER_NONE;
volatile uint8_t  s_session        = n2kupd::STATE_IDLE;
volatile bool     s_verify         = false;
volatile uint32_t s_verifyCapAt    = 0;
volatile bool     s_restartPending = false;

inline bool sessionBusy(uint8_t st) {
    return st >= n2kupd::STATE_CONFIRMING && st <= n2kupd::STATE_APPLYING;
}

}  // namespace

bool claimWriter(Owner o) {
    bool ok = false;
    GATE_LOCK();
    if (s_writer == OWNER_NONE || s_writer == o) {
        // A WiFi upload must not start in the middle of an N2K dialog either,
        // and nothing at all may write during the verify window or while a
        // restart is pending (11.5).
        const bool blocked = (o == OWNER_WIFI && sessionBusy(s_session)) ||
                             s_verify || s_restartPending;
        if (!blocked) { s_writer = o; ok = true; }
    }
    GATE_UNLOCK();
    return ok;
}

void releaseWriter(Owner o) {
    GATE_LOCK();
    if (s_writer == o) s_writer = OWNER_NONE;
    GATE_UNLOCK();
}

Owner writer() { return (Owner)s_writer; }

void setSessionState(uint8_t state) { s_session = state; }
uint8_t sessionState()              { return s_session; }
bool n2kSessionActive()             { return sessionBusy(s_session); }

void setVerifyWindow(bool on, uint32_t capAtMs) {
    GATE_LOCK();
    s_verify = on;
    s_verifyCapAt = capAtMs;
    GATE_UNLOCK();
}
bool verifyWindow() { return s_verify; }

void setRestartPending(bool on) { s_restartPending = on; }
bool restartPending()           { return s_restartPending; }

bool wifiOtaActive() { return s_writer == OWNER_WIFI; }

bool canStartWifiOta() {
    return !sessionBusy(s_session) && !s_verify && !s_restartPending &&
           s_writer == OWNER_NONE;
}

bool canRestart() { return !sessionBusy(s_session) && !s_verify; }

bool demoAllowed() { return !sessionBusy(s_session) && !s_verify; }

uint32_t restartBlockedSeconds() {
    GATE_LOCK();
    const bool v = s_verify;
    const uint32_t cap = s_verifyCapAt;
    GATE_UNLOCK();
    if (!v) return 0;
    const int32_t left = (int32_t)(cap - (uint32_t)millis());
    return left > 0 ? (uint32_t)(left + 999) / 1000 : 0;
}

const char *describe() {
    if (s_writer == OWNER_WIFI) return "wifi";
    if (sessionBusy(s_session)) return "n2k";
    if (s_verify)               return "verify";
    if (s_restartPending)       return "restart_pending";
    return "idle";
}

}  // namespace UpdateGate
