#pragma once
// ============================================================================
// UpdUi - the mailbox between the update path and the display task
// (docs/N2K-UPDATE.md 7.1).
//
// Every piece of update UI lives on lv_layer_top() in the display task; the
// N2K task never touches LVGL. The two talk through this small mailbox,
// guarded by a spinlock, in the same pattern as AnchorSyncNode's pending
// state:
//
//   N2K task -> UI   the receiver's n2kupd::IUpdUi calls (confirmOpen,
//                    stateChanged, failed, applied, identify ...) and the
//                    progress snapshots N2kUpdateNode posts while a session
//                    runs; OtaGuard's verify banner, success and rollback
//                    notices
//   UI -> N2K task   Install (held 1.5 s), Decline, Cancel (held 1.5 s)
//   UI -> device     Restart now, Later, rollback notice seen - through hooks
//                    the device glue installs (nothing to do in the simulator)
//
// Small on purpose, it is internal RAM: one ConfirmInfo slot, one snapshot
// slot, a FIFO of eight small events. Builds in the PC simulator too, where
// --n2kupdate-demo posts into it to drive the overlay.
// ============================================================================
#include <stdint.h>
#include "N2kUpdateCore.h"

namespace UpdUi {

// ---- events for the display task ------------------------------------------------
enum EventType : uint8_t {
    EV_NONE = 0,
    EV_CONFIRM_OPEN,      // payload: takeConfirm()
    EV_CONFIRM_CLOSE,     // token
    EV_FAILED,            // token, a = reason
    EV_APPLIED,           // token, a = target, b = mode, c = detail
    EV_IDENTIFY,          // a = seconds
    EV_VALIDATED,         // the new firmware was marked valid: success toast
    EV_ROLLBACK_NOTICE,   // payload: takeNotice()
};

struct Event {
    uint8_t  type;
    uint8_t  a, b, c;
    uint32_t token;
};

// The receiver's UI interface; every call is posted, none touches LVGL.
n2kupd::IUpdUi &receiverUi();

// Progress for the banner: the latest snapshot wins (N2K task).
void postSnapshot(const n2kupd::Snapshot &s);

// OtaGuard (10.2, 7.5): the verify banner, and the two notices.
void setVerify(bool active, uint16_t secondsLeft, const char *version);
void postValidated(const char *version);
struct Notice {
    uint8_t cause, detail;
    char    failedVersion[17];
    char    runningVersion[17];
};
void postRollbackNotice(const Notice &n);

// ---- display task -------------------------------------------------------------------
bool takeEvent(Event &out);
bool takeConfirm(n2kupd::ConfirmInfo &out);       // with EV_CONFIRM_OPEN
bool takeSnapshot(n2kupd::Snapshot &out);         // true if newer than the last take
bool peekVerify(uint16_t &secondsLeft, char version[17]);   // banner while it returns true
bool takeValidatedVersion(char version[17]);
bool takeNotice(Notice &out);

// UI -> receiver (drained by the N2K task).
enum UserCmd : uint8_t { CMD_NONE = 0, CMD_ACCEPT, CMD_DECLINE, CMD_CANCEL };
void userCommand(UserCmd cmd, uint32_t token);
bool takeUserCommand(UserCmd &cmd, uint32_t &token);
// The identify screen was tapped away (7.7): a flag of its own, so it can
// never overwrite an Install or a Cancel waiting in the command slot.
void noteIdentifyClosed();
bool takeIdentifyClosed();

// The restart countdown is on screen (IDENTIFY is ignored meanwhile, 3.11).
void setCountdownOpen(bool open);
bool countdownOpen();

// UI -> device actions without the receiver. Installed by the device glue;
// null in the simulator, where the calls only log.
struct Hooks {
    void (*restartNow)();     // countdown ran out, or "Restart now"
    void (*later)();          // "Later": the H5 marker switches to ANY_FRAME
    void (*noticeSeen)();     // the rollback notice was acknowledged
};
void setHooks(const Hooks &h);
void restartNow();
void later();
void noticeSeen();

}  // namespace UpdUi
