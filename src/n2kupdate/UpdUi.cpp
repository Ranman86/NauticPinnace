// ============================================================================
// UpdUi.cpp - see UpdUi.h.
// ============================================================================
#include "UpdUi.h"
#include <Arduino.h>
#include <string.h>

namespace UpdUi {
namespace {

// The simulator posts from the same thread that runs LVGL (sim_main.cpp's
// main loop), so the guard compiles away there - like AnchorSyncNode's.
#ifdef SIMULATOR
#define UI_LOCK()   do {} while (0)
#define UI_UNLOCK() do {} while (0)
#else
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define UI_LOCK()   portENTER_CRITICAL(&s_mux)
#define UI_UNLOCK() portEXIT_CRITICAL(&s_mux)
#endif

constexpr uint8_t QUEUE = 8;
Event    s_q[QUEUE];
uint8_t  s_qHead = 0, s_qCount = 0;

n2kupd::ConfirmInfo s_confirm;
bool                s_confirmValid = false;

n2kupd::Snapshot s_snap;
uint32_t         s_snapSeq = 0, s_snapTaken = 0;

bool     s_verifyActive = false;
uint16_t s_verifySecs = 0;
char     s_verifyVer[17] = { 0 };

char     s_validVer[17] = { 0 };
bool     s_validValid = false;

Notice   s_notice;
bool     s_noticeValid = false;

UserCmd  s_cmd = CMD_NONE;
uint32_t s_cmdToken = 0;
bool     s_identifyClosed = false;

volatile bool s_countdown = false;
Hooks    s_hooks = { nullptr, nullptr, nullptr };

// Call with the lock held. A full queue drops the new event: eight slots
// drained every 100 ms only fill up if the display task has stopped, and
// then nothing would show anyway.
void pushLocked(uint8_t type, uint32_t token, uint8_t a = 0, uint8_t b = 0, uint8_t c = 0) {
    if (s_qCount >= QUEUE) return;
    Event &e = s_q[(s_qHead + s_qCount) % QUEUE];
    e.type = type; e.token = token; e.a = a; e.b = b; e.c = c;
    s_qCount++;
}

void copyVersion(char dst[17], const char *src) {
    memset(dst, 0, 17);
    if (src) strncpy(dst, src, 16);
}

class PostingUi : public n2kupd::IUpdUi {
public:
    void confirmOpen(const n2kupd::ConfirmInfo &info) override {
        UI_LOCK();
        s_confirm = info;
        s_confirmValid = true;
        pushLocked(EV_CONFIRM_OPEN, info.token);
        UI_UNLOCK();
    }
    void confirmClose(uint32_t token) override {
        UI_LOCK(); pushLocked(EV_CONFIRM_CLOSE, token); UI_UNLOCK();
    }
    void stateChanged(const n2kupd::Snapshot &s) override { postSnapshot(s); }
    void failed(uint32_t token, uint8_t reason) override {
        UI_LOCK(); pushLocked(EV_FAILED, token, reason); UI_UNLOCK();
    }
    void applied(uint32_t token, uint8_t target, uint8_t mode, uint8_t detail) override {
        UI_LOCK(); pushLocked(EV_APPLIED, token, target, mode, detail); UI_UNLOCK();
    }
    void identify(uint8_t seconds) override {
        UI_LOCK(); pushLocked(EV_IDENTIFY, 0, seconds); UI_UNLOCK();
    }
};
PostingUi s_ui;

}  // namespace

n2kupd::IUpdUi &receiverUi() { return s_ui; }

void postSnapshot(const n2kupd::Snapshot &s) {
    UI_LOCK();
    s_snap = s;
    s_snapSeq++;
    UI_UNLOCK();
}

void setVerify(bool active, uint16_t secondsLeft, const char *version) {
    UI_LOCK();
    s_verifyActive = active;
    s_verifySecs = secondsLeft;
    copyVersion(s_verifyVer, version);
    UI_UNLOCK();
}

void postValidated(const char *version) {
    UI_LOCK();
    copyVersion(s_validVer, version);
    s_validValid = true;
    pushLocked(EV_VALIDATED, 0);
    UI_UNLOCK();
}

void postRollbackNotice(const Notice &n) {
    UI_LOCK();
    s_notice = n;
    s_noticeValid = true;
    pushLocked(EV_ROLLBACK_NOTICE, 0);
    UI_UNLOCK();
}

bool takeEvent(Event &out) {
    UI_LOCK();
    const bool have = s_qCount > 0;
    if (have) {
        out = s_q[s_qHead];
        s_qHead = (uint8_t)((s_qHead + 1) % QUEUE);
        s_qCount--;
    }
    UI_UNLOCK();
    return have;
}

bool takeConfirm(n2kupd::ConfirmInfo &out) {
    UI_LOCK();
    const bool have = s_confirmValid;
    if (have) out = s_confirm;
    UI_UNLOCK();
    return have;
}

bool takeSnapshot(n2kupd::Snapshot &out) {
    UI_LOCK();
    const bool fresh = s_snapSeq != s_snapTaken;
    if (fresh) { out = s_snap; s_snapTaken = s_snapSeq; }
    UI_UNLOCK();
    return fresh;
}

bool peekVerify(uint16_t &secondsLeft, char version[17]) {
    UI_LOCK();
    const bool a = s_verifyActive;
    secondsLeft = s_verifySecs;
    memcpy(version, s_verifyVer, 17);
    UI_UNLOCK();
    return a;
}

bool takeValidatedVersion(char version[17]) {
    UI_LOCK();
    const bool have = s_validValid;
    if (have) { memcpy(version, s_validVer, 17); s_validValid = false; }
    UI_UNLOCK();
    return have;
}

bool takeNotice(Notice &out) {
    UI_LOCK();
    const bool have = s_noticeValid;
    if (have) { out = s_notice; s_noticeValid = false; }
    UI_UNLOCK();
    return have;
}

void userCommand(UserCmd cmd, uint32_t token) {
    UI_LOCK();
    s_cmd = cmd;
    s_cmdToken = token;
    UI_UNLOCK();
}

bool takeUserCommand(UserCmd &cmd, uint32_t &token) {
    UI_LOCK();
    const bool have = s_cmd != CMD_NONE;
    if (have) { cmd = s_cmd; token = s_cmdToken; s_cmd = CMD_NONE; }
    UI_UNLOCK();
    return have;
}

void noteIdentifyClosed() {
    UI_LOCK();
    s_identifyClosed = true;
    UI_UNLOCK();
}

bool takeIdentifyClosed() {
    UI_LOCK();
    const bool have = s_identifyClosed;
    s_identifyClosed = false;
    UI_UNLOCK();
    return have;
}

void setCountdownOpen(bool open) { s_countdown = open; }
bool countdownOpen()             { return s_countdown; }

void setHooks(const Hooks &h) { s_hooks = h; }

void restartNow() {
    if (s_hooks.restartNow) s_hooks.restartNow();
    else Serial.println("[n2kupd] restart now (no hook: simulator)");
}
void later() {
    if (s_hooks.later) s_hooks.later();
    else Serial.println("[n2kupd] restart later (no hook: simulator)");
}
void noticeSeen() {
    if (s_hooks.noticeSeen) s_hooks.noticeSeen();
}

}  // namespace UpdUi
