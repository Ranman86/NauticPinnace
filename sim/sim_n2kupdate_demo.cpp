// ============================================================================
// sim_n2kupdate_demo.cpp - --n2kupdate-demo: the update over NMEA 2000 on the
// PC, without a bus.
//
// Plays the part of the N2K task: posts into the same UpdUi mailbox the
// receiver posts into on the device, and answers the overlay's Install /
// Decline / Cancel the way the receiver would. So the dialogs, the banner,
// the countdown and the notices can be looked at, clicked and held with the
// mouse, in every theme, language and orientation the simulator offers.
//
// Interactive: the confirmation dialog waits for the user. Holding "Install"
// starts a simulated transfer (about 20 s), then verification, activation,
// the restart countdown, the verify banner after the "restart" and the
// success toast; "Decline" ends the demo, "Cancel (hold)" on the banner
// shows the failure box. Then the identify screen and the rollback notice.
//
// With --shots DIR it walks the same states by itself and writes one BMP per
// state into DIR (upd_*.bmp), then quits. That walk also drags the anchor
// during the countdown (with demo data, i.e. a GPS fix): the countdown then
// waits for the alarm as a strip under the alarm banner.
// ============================================================================
#include "sim_hal.h"
#include "arduino_stubs.h"
#include "../src/n2kupdate/UpdUi.h"
#include "../src/n2kupdate/N2kUpdateProto.h"
#include "../src/config/Config.h"
#include "../src/nmea/DataModel.h"
#include "../src/display/UiConfig.h"   // UI_NAV_FADE_* - the --shots lead time
#include "../src/Version.h"
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace n2kupd;

namespace {

enum Phase {
    P_START, P_CONFIRM, P_TRANSFER, P_VERIFY, P_APPLIED, P_COUNTDOWN, P_COUNTDOWN_ALARM,
    P_COUNTDOWN_END, P_BOOT_VERIFY, P_VALID, P_FAILED, P_IDENTIFY, P_NOTICE, P_WEBUI, P_END,
};

// An anchor 200 m from the demo boat's position, with the watch armed: the
// real drag alarm of DisplayManager fires. False without a GPS fix (--nodemo).
bool dragAnchor(bool on) {
    if (!on) {
        appConfig.cfg.anchorAlarmOn = false;
        appConfig.cfg.anchorSet = false;
        return true;
    }
    float lat, lon;
    { auto lk = data.lock(); lat = data.lat; lon = data.lon; }
    if (std::isnan(lat) || std::isnan(lon)) return false;
    appConfig.cfg.anchorSet = true;
    appConfig.cfg.anchorLat = lat + 200.0f / (60.0f * 1852.0f);
    appConfig.cfg.anchorLon = lon;
    appConfig.cfg.anchorRadius = 40.0f;
    appConfig.cfg.anchorAlarmOn = true;
    return true;
}

Phase    s_phase = P_START;
uint32_t s_t0 = 0;           // when the current phase began
uint32_t s_token = 0;
uint32_t s_xferStart = 0;
int      s_shot = 0;
bool     s_shotTaken = false;

constexpr uint32_t IMAGE  = 2441792;    // a 7B firmware.bin
constexpr uint32_t STREAM = 1313452;    // its deflate stream

// The version on offer: always the next minor release after the running
// firmware, so the dialog shows a real upgrade whatever FW_VERSION is. A fixed
// "1.3.0" turned into "(same version)" the day 1.3.0 itself was released.
const char *demoNewVersion() {
    static char v[17];
    if (!v[0]) {
        int major = 0, minor = 0;
        sscanf(FW_VERSION, "%d.%d", &major, &minor);
        snprintf(v, sizeof(v), "%d.%d.0", major, minor + 1);
    }
    return v;
}

ConfirmInfo demoOffer(uint32_t token) {
    ConfirmInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.token = token;
    ci.senderAddress = 230;
    putUtf8Field(ci.senderName, "PC Updater");
    ci.senderKind = SENDER_UPDATER;
    ci.target = TARGET_FIRMWARE;
    // The board the simulator stands in for: an OFFER for another board never
    // reaches the dialog on a real display (WRONG_BOARD).
#if defined(BOARD_PANEL_1024X600)
    ci.boardId = BOARD_7B;
#else
    ci.boardId = BOARD_4INCH;
#endif
    ci.encoding = ENC_DEFLATE;
    putVersionField(ci.runningVersion, FW_VERSION);
    putVersionField(ci.version, demoNewVersion());
    ci.versionRelation = (int8_t)compareVersions(demoNewVersion(), FW_VERSION);
    ci.imageSize = IMAGE;
    ci.streamSize = STREAM;
    ci.companionSize = 48751;            // the release's web interface
    ci.estSeconds = 360;
    ci.dialogSeconds = 60;
    return ci;
}

void post(uint8_t state, uint32_t written, uint32_t accepted, uint16_t secs, uint32_t now) {
    Snapshot s;
    memset(&s, 0, sizeof(s));
    s.token = s_token;
    s.state = state;
    s.target = TARGET_FIRMWARE;
    s.encoding = ENC_DEFLATE;
    s.senderAddress = 230;
    s.sessionId = 0x5A;
    putVersionField(s.version, demoNewVersion());
    s.imageWritten = written;
    s.imageSize = IMAGE;
    s.streamAccepted = accepted;
    s.streamSize = STREAM;
    s.transferStartMs = s_xferStart;
    s.secondsLeft = secs;
    UpdUi::postSnapshot(s);
    (void)now;
}

void go(Phase p, uint32_t now) {
    s_phase = p;
    s_t0 = now;
    s_shotTaken = false;
}

// In --shots mode: one picture per phase, `afterMs` into it.
bool shotDue(const char *dir, const char *name, uint32_t now, uint32_t afterMs) {
    if (!dir || s_shotTaken || now - s_t0 < afterMs) return false;
    char path[512];
    snprintf(path, sizeof(path), "%s/upd_%02d_%s.bmp", dir, s_shot++, name);
    sim_hal_screenshot(path);
    printf("[shots] %s\n", path);
    s_shotTaken = true;
    return true;
}

}  // namespace

// ---- --n2kupdate-selftest ----------------------------------------------------------
// The press-and-hold rules of 7.1, through real (injected) mouse events on
// the real dialog:
//   1. a press that begins inside the 1.5 s guard, held for 2 s: nothing
//   2. a short tap after the guard: nothing
//   3. a press after the guard, held for 1.7 s: Install
// Returns -1 while running, 0 on PASS, 1 on FAIL.
namespace {

void mouse(uint32_t type, int x, int y) {
    SDL_Event e{};
    e.type = type;
    if (type == SDL_MOUSEMOTION) { e.motion.x = x; e.motion.y = y; }
    else { e.button.button = SDL_BUTTON_LEFT; e.button.x = x; e.button.y = y; }
    SDL_PushEvent(&e);
}

}  // namespace

int simUpdHoldSelfTest(uint32_t now, lv_obj_t *installBtn) {
    static int step = 0;
    static uint32_t t0 = 0, tStep = 0;
    static int bx = 0, by = 0;
    UpdUi::UserCmd cmd = UpdUi::CMD_NONE;
    uint32_t tok = 0;
    const bool got = UpdUi::takeUserCommand(cmd, tok);
    const bool accepted = got && cmd == UpdUi::CMD_ACCEPT;
    auto fail = [](const char *why) { printf("[upd-selftest] FAIL: %s\n", why); return 1; };

    switch (step) {
        case 0:                                   // let the screens settle
            if (!t0) t0 = now;
            if (now - t0 < 1500) return -1;
            s_token = 77;
            UpdUi::receiverUi().confirmOpen(demoOffer(s_token));
            t0 = now;
            step = 1;
            return -1;
        case 1:                                   // the dialog is up: find the button
            if (!installBtn) { if (now - t0 > 2000) return fail("dialog did not open"); return -1; }
            {
                // The dialog was built a moment ago; its flex layout has not
                // run yet, so the coordinates would still be wrong.
                lv_obj_update_layout(installBtn);
                lv_area_t a;
                lv_obj_get_coords(installBtn, &a);
                bx = (a.x1 + a.x2) / 2;
                by = (a.y1 + a.y2) / 2;
            }
            printf("[upd-selftest] Install button at %d,%d\n", bx, by);
            mouse(SDL_MOUSEMOTION, bx, by);
            mouse(SDL_MOUSEBUTTONDOWN, bx, by);   // inside the guard (~0.3 s after opening)
            tStep = now;
            step = 2;
            return -1;
        case 2:                                   // ...held for 2 s: must not install
            if (accepted) return fail("a hold that began inside the guard installed");
            if (now - tStep < 2000) return -1;
            mouse(SDL_MOUSEBUTTONUP, bx, by);
            tStep = now;
            step = 3;
            return -1;
        case 3:                                   // a short tap after the guard
            if (accepted) return fail("installed on release");
            if (now - tStep < 300) return -1;
            mouse(SDL_MOUSEBUTTONDOWN, bx, by);
            tStep = now;
            step = 4;
            return -1;
        case 4:
            if (now - tStep < 150) return -1;
            mouse(SDL_MOUSEBUTTONUP, bx, by);
            tStep = now;
            step = 5;
            return -1;
        case 5:
            if (accepted) return fail("a short tap installed");
            if (now - tStep < 500) return -1;
            printf("[upd-selftest] guard and short tap ignored - PASS so far\n");
            mouse(SDL_MOUSEBUTTONDOWN, bx, by);   // the real hold
            tStep = now;
            step = 6;
            return -1;
        case 6:
            if (accepted) {
                const uint32_t held = now - tStep;
                mouse(SDL_MOUSEBUTTONUP, bx, by);
                if (tok != s_token) return fail("accept carried the wrong token");
                if (held < n2kupd::T_HOLD_MS) return fail("installed before 1.5 s");
                printf("[upd-selftest] installed after a %u ms hold - PASS\n", (unsigned)held);
                return 0;
            }
            if (now - tStep > 3500) return fail("a 3.5 s hold did not install");
            return -1;
        default:
            return 1;
    }
}

// Called from the simulator's main loop (the LVGL thread). Returns false
// once the demo is over.
bool simUpdDemoTick(uint32_t now, const char *shotsDir) {
    UpdUi::UserCmd cmd = UpdUi::CMD_NONE;
    uint32_t tok = 0;
    const bool haveCmd = UpdUi::takeUserCommand(cmd, tok);
    if (haveCmd) printf("[upd-demo] user command %d (token %u)\n", (int)cmd, (unsigned)tok);

    switch (s_phase) {
        case P_START: {
            // Interactive, the offer comes 1.5 s in. A --shots run waits until
            // the translucent nav arrows and the gear have faded out (their
            // idle delay plus the fade, the same uiSz values DisplayManager
            // runs on): nobody touches the screen to bring them back, so no
            // picture of the set shows them over the instrument - least of
            // all the progress banner, which is taken only ~4 s in.
            const uint32_t lead = shotsDir
                ? (uint32_t)UI_NAV_FADE_DELAY_MS + (uint32_t)UI_NAV_FADE_OUT_MS + 1000u
                : 1500u;
            if (now - s_t0 < lead && s_t0) break;
            if (!s_t0) { s_t0 = now; break; }
            s_token++;
            {
                const ConfirmInfo ci = demoOffer(s_token);
                UpdUi::receiverUi().confirmOpen(ci);
            }
            post(STATE_CONFIRMING, 0, 0, 60, now);
            printf("[upd-demo] OFFER shown - hold \"Install\" for 1.5 s, or decline\n");
            go(P_CONFIRM, now);
            break;
        }

        case P_CONFIRM: {
            bool accepted = haveCmd && cmd == UpdUi::CMD_ACCEPT;
            if (shotsDir) {
                // Nobody holds "Install" in a --shots run: the picture shows
                // the dialog, then the receiver's side moves on as if accepted.
                if (shotDue(shotsDir, "confirm", now, 1200)) break;
                if (!s_shotTaken || now - s_t0 <= 1600) break;
                UpdUi::receiverUi().confirmClose(s_token);
                accepted = true;
            }
            if (accepted) {
                s_xferStart = now;
                post(STATE_PREPARING, 0, 0, 0xFFFF, now);
                go(P_TRANSFER, now);
            } else if (haveCmd && cmd == UpdUi::CMD_DECLINE) {
                printf("[upd-demo] declined\n");
                UpdUi::receiverUi().identify(8);
                go(P_IDENTIFY, now);
            } else if (now - s_t0 > 61000) {
                UpdUi::receiverUi().confirmClose(s_token);   // CONFIRM_TIMEOUT
                UpdUi::receiverUi().identify(8);
                go(P_IDENTIFY, now);
            }
            break;
        }

        case P_TRANSFER: {
            if (haveCmd && cmd == UpdUi::CMD_CANCEL) {
                UpdUi::receiverUi().failed(s_token, RSN_USER_CANCELLED);
                post(STATE_FAILED, 0, 0, 0xFFFF, now);
                go(P_FAILED, now);
                break;
            }
            // 20 s for the whole image here (about 6 min on a real bus); in
            // --shots mode the banner is captured at 42 % with a believable
            // remaining time.
            const uint32_t dur = 20000;
            uint32_t e = now - s_t0;
            if (shotsDir) {
                s_xferStart = now - 140000;                 // 2 min 20 s in
                post(STATE_RECEIVING, IMAGE * 42 / 100, STREAM * 42 / 100, 0xFFFF, now);
                if (shotDue(shotsDir, "progress", now, 800)) break;
                if (s_shotTaken) go(P_VERIFY, now);
                break;
            }
            if (e > dur) e = dur;
            post(e < 600 ? STATE_PREPARING : (e / 400) % 2 ? STATE_PROCESSING : STATE_RECEIVING,
                 (uint32_t)((uint64_t)IMAGE * e / dur), (uint32_t)((uint64_t)STREAM * e / dur),
                 0xFFFF, now);
            if (now - s_t0 >= dur) go(P_VERIFY, now);
            break;
        }

        case P_VERIFY:
            post(STATE_VERIFYING, IMAGE, STREAM, 5, now);
            if (shotsDir) { if (shotDue(shotsDir, "verifying", now, 800)) break;
                            if (!s_shotTaken) break; }
            else if (now - s_t0 < 3000) break;
            post(STATE_APPLYING, IMAGE, STREAM, 0xFFFF, now);
            go(P_APPLIED, now);
            break;

        case P_APPLIED:
            if (now - s_t0 < 400) break;
            post(STATE_DONE, IMAGE, STREAM, 0xFFFF, now);
            UpdUi::receiverUi().applied(s_token, TARGET_FIRMWARE, APPLY_NOW, 0);
            go(P_COUNTDOWN, now);
            break;

        case P_COUNTDOWN:
            // The overlay runs the 10 s itself and then calls the restart hook,
            // which only logs on the PC.
            if (shotsDir) {
                if (shotDue(shotsDir, "countdown", now, 1200)) break;
                if (!s_shotTaken) break;
                // The anchor drags: the countdown waits for the alarm (5.6).
                if (dragAnchor(true)) { go(P_COUNTDOWN_ALARM, now); break; }
            }
            if (now - s_t0 < 11500u) break;
            post(STATE_IDLE, 0, 0, 0xFFFF, now);
            go(P_BOOT_VERIFY, now);
            break;

        case P_COUNTDOWN_ALARM:
            if (shotDue(shotsDir, "countdown_alarm", now, 1500)) break;
            if (!s_shotTaken) break;
            dragAnchor(false);                     // alarm over: the countdown starts again
            go(P_COUNTDOWN_END, now);
            break;

        case P_COUNTDOWN_END:
            if (now - s_t0 < 11500u) break;
            post(STATE_IDLE, 0, 0, 0xFFFF, now);
            go(P_BOOT_VERIFY, now);
            break;

        case P_BOOT_VERIFY: {
            // "After the restart": the verify banner of the OtaGuard window.
            const uint32_t e = now - s_t0;
            const uint16_t left = e < 6000 ? (uint16_t)(58 - e / 1000) : 52;
            UpdUi::setVerify(true, left, demoNewVersion());
            if (shotsDir) { if (shotDue(shotsDir, "verify_banner", now, 1200)) break;
                            if (!s_shotTaken) break; }
            else if (e < 6000) break;
            UpdUi::setVerify(false, 0, demoNewVersion());
            UpdUi::postValidated(demoNewVersion());
            go(P_VALID, now);
            break;
        }

        case P_VALID:
            if (shotsDir && shotDue(shotsDir, "success", now, 800)) break;
            if (now - s_t0 < 5500) break;              // the toast stays 5 s
            // A failed session next: the box of 7.6.
            s_token++;
            UpdUi::receiverUi().failed(s_token, RSN_SHA_MISMATCH);
            go(P_FAILED, now);
            break;

        case P_FAILED:
            if (shotsDir) { if (shotDue(shotsDir, "failed", now, 800)) break;
                            if (!s_shotTaken) break; }
            else if (now - s_t0 < 8000) break;
            go(P_IDENTIFY, now);
            UpdUi::receiverUi().identify(shotsDir ? 3 : 8);
            break;

        case P_IDENTIFY:
            if (shotsDir) { if (shotDue(shotsDir, "identify", now, 800)) break;
                            if (!s_shotTaken) break; }
            if (now - s_t0 < (shotsDir ? 3500u : 9000u)) break;
            {
                UpdUi::Notice n;
                memset(&n, 0, sizeof(n));
                n.cause = RB_HEALTH;
                n.detail = 5;
                snprintf(n.failedVersion, sizeof(n.failedVersion), "%s", demoNewVersion());
                strncpy(n.runningVersion, FW_VERSION, 16);
                UpdUi::postRollbackNotice(n);
            }
            go(P_NOTICE, now);
            break;

        case P_NOTICE:
            if (shotsDir) { if (shotDue(shotsDir, "rollback_notice", now, 800)) break;
                            if (!s_shotTaken) break; }
            else if (now - s_t0 < 8000) break;
            UpdUi::receiverUi().applied(s_token, TARGET_WEBUI, APPLY_NOW, 0);
            go(P_WEBUI, now);
            break;

        case P_WEBUI:
            if (shotsDir) { if (shotDue(shotsDir, "webui_toast", now, 800)) break;
                            if (!s_shotTaken) break; }
            else if (now - s_t0 < 6000) break;
            printf("[upd-demo] done\n");
            go(P_END, now);
            break;

        case P_END:
        default:
            return false;
    }
    return true;
}
