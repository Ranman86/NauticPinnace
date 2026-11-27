// ============================================================================
// OtaGuard.cpp - see OtaGuard.h and docs/N2K-UPDATE.md 10.2-10.6.
// ============================================================================
#ifndef SIMULATOR
#include "OtaGuard.h"
#include "UpdateGate.h"
#include "UpdRegions.h"
#include "UpdNvs.h"
#include "UpdUi.h"
#include "UpdCrypto.h"
#include "N2kUpdateProto.h"
#include "../Version.h"
#include "../LoopWatchdog.h"   // owns the RTC watchdog the backstop runs on
#include "../UsbConsole.h"     // consoleFlushFull() before the rollback restart
#include <Arduino.h>
#include <esp_attr.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_idf_version.h>
#include <esp_private/system_internal.h>   // esp_restart_noos(): rollBack()
#include <string.h>

// THE switch that makes rollback work (10.1, 10.2). Arduino's initArduino()
// confirms a PENDING_VERIFY image before setup() unless this returns true.
// C linkage is mandatory: the weak default lives in esp32-hal-misc.c and no
// header declares it, so a plain C++ definition would be name-mangled and
// silently ignored - and every update would be confirmed unseen, as before.
extern "C" bool verifyRollbackLater() { return true; }

namespace OtaGuard {

volatile uint32_t g_lvFlushCount = 0;
volatile uint32_t g_n2kLoopCount = 0;
volatile bool     g_anyN2kFrame  = false;

namespace {

// ---- timers of 10.2 / 10.3 / 10.6 (normative values, 5.11) ---------------------------
constexpr uint32_t T_SETUP_MAX_MS     = 90000;    // H1
constexpr uint32_t T_STALL_MS         = 5000;     // H2-H4: "advanced within 5 s"
constexpr uint32_t T_FAIL_MS          = 15000;    // H2-H4 failing continuously
constexpr uint32_t T_EARLIEST_MS      = 60000;    // earliest valid, after setupDone
constexpr uint32_t T_CAP_MS           = 180000;   // latest decision, after setupDone
constexpr uint32_t T_BUS_DOWN_MAX_MS  = 20000;    // H4: longer than a recovery with back-off
constexpr uint32_t BACKSTOP_MS        = 300000;   // RTC watchdog (10.3)
constexpr uint32_t BACKSTOP_ROLLBACK_MS = 10000;  // ... around OtaGuard's own rollback restart
constexpr uint32_t CLG_WINDOW_S       = 1800;     // crash-loop guard: 30 min of uptime
constexpr uint32_t CLG_WRITE_MS       = 60000;
constexpr uint8_t  CLG_MAX_CRASHES    = 3;
constexpr uint32_t COMPANION_MS       = n2kupd::T_COMPANION_USABLE_MS;

// ---- what the running image is -----------------------------------------------------
const esp_partition_t *s_running = nullptr;
uint8_t  s_slot      = 0xFF;
uint8_t  s_id[8]     = { 0 };
bool     s_haveState = false;
esp_ota_img_states_t s_state = ESP_OTA_IMG_UNDEFINED;
bool     s_anchored  = false;

// ---- the verify window -------------------------------------------------------------
volatile bool s_pending  = false;       // PENDING_VERIFY and not decided yet
volatile bool s_valid    = false;       // marked valid in THIS boot
volatile bool s_setupOk  = false;
uint32_t s_bootMs        = 0;
uint32_t s_setupMs       = 0;
uint32_t s_validAtMs     = 0;
bool     s_rbPossible    = true;
esp_timer_handle_t s_timer = nullptr;

bool     s_h5Required = false;
uint8_t  s_h5Mode     = UpdNvs::H5_PROTOCOL;
volatile bool s_selfTestsOk = false;
volatile bool s_n2kTask     = false;   // the N2K task was created (H4 applies)
bool (*s_proofFn)()             = nullptr;
bool (*s_busStuckFn)(uint32_t&) = nullptr;
volatile uint32_t s_loopCount  = 0;
bool     s_markFailLogged = false;
bool     s_clgPrepared    = false;      // markValid() stored the crash-loop record

struct Watch { uint32_t last, changedAt; };
Watch    s_wLoop = { 0, 0 }, s_wFlush = { 0, 0 }, s_wN2k = { 0, 0 };
uint32_t s_failSince[5] = { 0, 0, 0, 0, 0 };    // per criterion H1..H5, 0 = not failing

// ---- rollback report (10.5) ----------------------------------------------------------
UpdNvs::RbInfo s_rb;
volatile bool  s_rbValid = false;
bool           s_noticePending = false;

// ---- crash-loop guard (10.6) ------------------------------------------------------------
// Only this build reads it, so an RTC_NOINIT record is fine here; anything
// another image must read goes through NVS.
struct CrashRec { uint32_t magic; uint8_t id[8]; uint8_t count; uint8_t assertFlag; };
constexpr uint32_t CRASH_MAGIC = 0x4E50434CUL;   // "NPCL"
RTC_NOINIT_ATTR CrashRec s_crash;
bool     s_clgActive  = false;
uint32_t s_clgBaseS   = 0;          // seconds stored before this boot / mark-valid
uint32_t s_clgFromMs  = 0;          // millis() the base refers to
uint32_t s_clgWriteMs = 0;
uint8_t  s_clgOther[8] = { 0 };     // the rollback target, as recorded at mark-valid

// ---- companion web UI (5.3.6) --------------------------------------------------------------
UpdNvs::PreWeb s_preweb;
volatile bool  s_prewebLoaded = false;

// What bootEarly() did, printed by bootReport() once Serial is up.
char s_report[192] = { 0 };

void note(const char *s) {
    const size_t n = strlen(s_report);
    if (n + strlen(s) + 2 < sizeof(s_report)) { strcat(s_report, s); strcat(s_report, "\n"); }
}

// The hang backstop (10.3): never fed. It fires only if the image is neither
// confirmed nor rolled back within 300 s - which takes a hang that has also
// stopped the esp_timer task. Armed again with 10 s just before OtaGuard's
// own rollback restart, so a hang in there cannot leave the display frozen.
// The RTC watchdog belongs to LoopWatchdog, which programs it in two stages:
// a system reset at `ms`, and 2 s later an RTC reset. Measured on all three
// boards, the first stage already ends as an RTC reset (rst 0x10, which also
// boots a chip whose core 0 a panic handler had stalled); the second is a
// fallback. Either reads as ESP_RST_WDT. At mark-valid the backstop is not
// switched off but handed over: it keeps running until the next loop() pass
// turns it into the loop watch.
void backstopArm(uint32_t ms) { LoopWatchdog::armBackstop(ms); }

// 4.11: how the rolled-back image ended, from the reset reason this boot sees.
uint8_t causeFromResetReason(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_PANIC: case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT:
            return n2kupd::RB_CRASH;
        case ESP_RST_POWERON: case ESP_RST_BROWNOUT:
            return n2kupd::RB_POWER;
        default:
            return n2kupd::RB_RESTART;
    }
}

bool isCrashReset(esp_reset_reason_t r) {
    return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
}

void stopTimer() {
    if (s_timer) esp_timer_stop(s_timer);
}

// The image id at +0xB0 of the other OTA slot, read through the bounce buffer
// (its owner at boot and in the verify window, when nothing writes).
bool otherSlotId(uint8_t out[8]) {
    const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
    if (!other) return false;
    uint8_t *b = UpdRegions::bounce();
    if (esp_partition_read(other, n2kupd::IMAGE_ID_OFFSET, b, 8) != ESP_OK) return false;
    memcpy(out, b, 8);
    return true;
}

bool isAllFF8(const uint8_t id[8]) {
    for (int i = 0; i < 8; i++) if (id[i] != 0xFF) return false;
    return true;
}

// ---- 10.6 at boot -------------------------------------------------------------------------
void crashLoopCheck() {
    UpdNvs::Clg clg;
    if (!UpdNvs::loadClg(clg)) {
        s_crash.magic = 0;            // not in the window: forget any old count
        return;
    }
    if (memcmp(clg.imageId, s_id, 8) != 0) {
        // The window of another image - one that was rolled back, or replaced
        // over USB. Nothing to guard any more.
        UpdNvs::clearClg();
        s_crash.magic = 0;
        return;
    }
    if (s_crash.magic != CRASH_MAGIC || memcmp(s_crash.id, s_id, 8) != 0) {
        s_crash.magic = CRASH_MAGIC;
        memcpy(s_crash.id, s_id, 8);
        s_crash.count = 0;
        s_crash.assertFlag = 0;
    }
    const bool crashed = isCrashReset(esp_reset_reason()) || s_crash.assertFlag;
    s_crash.assertFlag = 0;
    s_crash.count = crashed ? (uint8_t)(s_crash.count + 1) : 0;
    if (s_crash.count >= CLG_MAX_CRASHES) {
        s_crash.count = 0;
        UpdNvs::Cause c;
        memcpy(c.imageId, s_id, 8);
        c.cause = n2kupd::RB_CRASH_LOOP;
        c.detail = 0;
        UpdNvs::storeCause(c);
        UpdNvs::clearClg();
        // Only back into the image this one replaced. An update into the other
        // slot that got past esp_ota_end() and was then refused or abandoned
        // leaves a complete image there behind an otadata entry that still
        // looks valid - never activated, maybe for another board. The
        // bootloader would start it as VALID, with no check window at all.
        uint8_t other[8];
        const bool same = otherSlotId(other) && !isAllFF8(clg.otherId) &&
                          memcmp(other, clg.otherId, 8) == 0;
        if (same && esp_ota_check_rollback_is_possible()) {
            // Works for a VALID image as well; does not return on success.
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
        note(same ? "[ota] crash-loop guard: rollback not possible - window ended"
                  : "[ota] crash-loop guard: the other slot was overwritten - window ended");
        UpdNvs::clearCause();
        return;
    }
    s_clgActive = true;
    s_clgBaseS  = clg.seconds;
    memcpy(s_clgOther, clg.otherId, 8);
    s_clgFromMs = s_bootMs;
    if (crashed) note("[ota] crash-loop guard: crash reset counted");
}

// ---- 10.5 at boot ---------------------------------------------------------------------------
void rollbackDetect() {
    const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
    if (!other) return;
    esp_ota_img_states_t ost;
    if (esp_ota_get_state_partition(other, &ost) != ESP_OK) return;
    if (ost != ESP_OTA_IMG_INVALID && ost != ESP_OTA_IMG_ABORTED) return;

    // The other slot's image id, through the bounce buffer (nothing else
    // runs yet). otadata alone is read for the state - no image validation
    // at every boot, unlike esp_ota_get_last_invalid_partition().
    uint8_t *b = UpdRegions::bounce();
    uint8_t id[8];
    if (esp_partition_read(other, n2kupd::IMAGE_ID_OFFSET, b, 8) != ESP_OK) return;
    memcpy(id, b, 8);
    // The otadata entry still describes an earlier rollback, but the slot has
    // been written since: erased by an abandoned update (OtaWriter::abort),
    // or an update was cut off mid-write. What is there now was never
    // started, so it is no rollback to report.
    if (isAllFF8(id) || UpdNvs::slotDirty()) return;

    // New rollback, or the one already reported? A cause record that names
    // this image id was written by that image itself right before it rolled
    // back (rollBack(): cause 1, crashLoopCheck(): cause 5), and is deleted
    // below as soon as it has been read - so while it exists, this IS a new
    // rollback, even when rbinfo names the same id already. That is the case
    // when a build that was rolled back once is installed and rolled back
    // again (on the bench: the same image in both slots). The id match alone
    // used to count as "known": the first rollback's cause and detail stayed,
    // and the new record was never read nor deleted - /api/info showed cause
    // 1 after a crash-loop rollback (cause 5). Rollbacks without a record (a
    // panic, a watchdog or a power cut in the verify window: the cause comes
    // from the reset reason) are new when the id differs; for a re-installed
    // build, otherSlotOverwritten() deletes rbinfo when the update starts.
    UpdNvs::Cause c;
    const bool haveCause = UpdNvs::loadCause(c) && memcmp(c.imageId, id, 8) == 0;
    UpdNvs::RbInfo rb;
    const bool known = !haveCause && UpdNvs::loadRbInfo(rb) && memcmp(rb.imageId, id, 8) == 0;
    if (!known) {
        memset(&rb, 0, sizeof(rb));
        memcpy(rb.imageId, id, 8);
        if (esp_partition_read(other, n2kupd::FWDESC_OFFSET, b, n2kupd::FWDESC_SIZE) == ESP_OK) {
            n2kupd::FwDesc d;
            if (n2kupd::parseDescriptor(b, n2kupd::FWDESC_SIZE, d)) memcpy(rb.version, d.fwVersion, 16);
        }
        if (haveCause) {
            rb.cause = c.cause;       // 1 (health check) or 5 (crash loop): recorded before
            rb.detail = c.detail;
        } else {
            rb.cause = causeFromResetReason(esp_reset_reason());
            rb.detail = 0;
        }
        UpdNvs::storeRbInfo(rb);
        // A new rbinfo gets its notice (10.5: once per rbinfo), also when an
        // earlier rollback of the same id was acknowledged already.
        UpdNvs::clearRbSeen();
        // The rolled-back image's H5 marker would otherwise outlive it
        // (deleted only at mark-valid). The companion approval is gone anyway:
        // bootEarly() deletes it at every boot.
        UpdNvs::clearH5();
        // The cause record goes LAST: while it exists, the next boot does all
        // of the above again, so a power cut in between loses nothing. Deleted
        // before rbseen, a cut between the two left the new report "known"
        // and its notice already acknowledged.
        UpdNvs::clearCause();
        note("[ota] the previous update was rolled back");
    }
    s_rb = rb;
    s_rbValid = true;
    uint8_t seen[8];
    s_noticePending = !(UpdNvs::loadRbSeen(seen) && memcmp(seen, rb.imageId, 8) == 0);
}

// ---- decisions ----------------------------------------------------------------------------------
// False if otadata could not be written: the image is then still
// PENDING_VERIFY, and everything stays as it was for the next attempt.
bool markValid() {
    // 10.6 first: the crash-loop window, with the image a rollback would go
    // back to. Stored before otadata changes, so a crash in between cannot
    // leave a validated image without its guard.
    if (!s_clgPrepared) {             // once: a retry must not wear NVS
        UpdNvs::Clg clg;
        memcpy(clg.imageId, s_id, 8);
        clg.seconds = 0;
        if (!otherSlotId(clg.otherId)) memset(clg.otherId, 0xFF, 8);
        memcpy(s_clgOther, clg.otherId, 8);
        UpdNvs::storeClg(clg);
        s_clgPrepared = true;
    }
    const esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    if (e != ESP_OK) {
        // The next reset would roll this image back. Keep the guard, the
        // backstop and the verify window, and try again at the next check.
        if (!s_markFailLogged) {
            Serial.printf("[ota] marking valid failed (%s) - retrying\n", esp_err_to_name(e));
            s_markFailLogged = true;
        }
        return false;
    }
    stopTimer();
    // Not disarmed: handed over. Should the loop be dead after all - this is
    // also the "rollback not possible" path of rollBack(), H1 or H2 failing -
    // the backstop still fires, and the next boot names that case; with a
    // live loop the next pass makes it the loop watch. This comes at most
    // ~270 s after bootEarly() (H1 90 s + T_CAP 180 s) against the backstop's
    // 300 s, which LoopWatchdog converts with the boot's calibration of the
    // RC slow clock: the margin holds while that clock stays within 10 % of
    // its calibration.
    LoopWatchdog::backstopToLoop();
    s_pending = false;
    s_valid = true;
    s_validAtMs = millis();
    UpdNvs::clearH5();
    // The crash count starts at zero for this image: an assert during the
    // check window, or a count left by an earlier install of the same build,
    // does not carry over.
    s_crash.magic = CRASH_MAGIC;
    memcpy(s_crash.id, s_id, 8);
    s_crash.count = 0;
    s_crash.assertFlag = 0;
    // Set last: loopTick() reads the window once s_clgActive is true.
    s_clgBaseS   = 0;
    s_clgFromMs  = s_validAtMs;
    s_clgWriteMs = s_validAtMs;
    s_clgActive  = true;
    UpdateGate::setVerifyWindow(false, 0);
    UpdUi::setVerify(false, 0, FW_VERSION);
    UpdUi::postValidated(FW_VERSION);
    Serial.printf("[ota] marked valid after %u s\n", (unsigned)((s_validAtMs - s_setupMs) / 1000));
    return true;
}

void rollBack(uint8_t criterion) {
    if (!s_rbPossible) {
        // After anchoring this only happens when the other slot holds no
        // valid image: there is nothing to go back to. (Should marking fail,
        // the guard tries again at its next check.)
        if (!s_markFailLogged)
            Serial.printf("[ota] health check H%u failed, but rollback is not possible (no valid "
                          "image in the other slot) - marking valid instead\n", (unsigned)criterion);
        markValid();
        return;
    }
    stopTimer();
    Serial.printf("[ota] health check H%u failed - rolling back\n", (unsigned)criterion);
    // Recorded first, so the old firmware can name the cause. If this write
    // fails it reports "another restart" (cause 4) instead.
    UpdNvs::Cause c;
    memcpy(c.imageId, s_id, 8);
    c.cause = n2kupd::RB_HEALTH;
    c.detail = criterion;
    UpdNvs::storeCause(c);
    // The backstop is not switched off for the restart but shortened: should
    // anything below hang, it is what resets the device: TG1 is off, and the
    // IDF task watchdog - still live with WiFi off on any board, and for
    // async_tcp on the 4" - only catches some hangs. The restart's reset
    // leaves the RTC watchdog running (lib/NMEA2000_esp32), and the bootloader
    // reprograms it first thing.
    backstopArm(BACKSTOP_ROLLBACK_MS);
    consoleFlushFull();   // the line above is the last one before the reset
    // A PENDING_VERIFY image needs no otadata write to be rolled back: the
    // bootloader marks an image that was not confirmed ABORTED at the next
    // boot and starts the previous one (10.1). This avoids
    // esp_ota_mark_app_invalid_rollback_and_reboot(), which validates the
    // other image once more and rewrites otadata here, in the esp_timer task
    // (4 KB of stack on the 4-inch). rollbackDetect() accepts ABORTED.
    //
    // The restart skips esp_restart()'s shutdown handlers: esp_restart_noos()
    // is linked to lib/NMEA2000_esp32's __wrap_esp_restart_noos
    // (extra_script.py, every board), the reset that esp_restart() and every
    // panic end in anyway - TX pin parked, interrupts off, digital system
    // reset. Found on the 7B bench (10.11.2026, health-check test A, 2 runs of
    // 2): with loop() hung and the BSH task's HTTPS download in flight (the web
    // dead from that moment, the internal heap at a few hundred bytes in such
    // downloads), "health check H3/H2 failed - rolling back" was the last line;
    // only this backstop reset the chip, 10.05 s later. The same test after the
    // download restarted at once. Between that line and the reset, storeCause()
    // had finished (the old image reported its cause 1 / detail 3) and so had
    // backstopArm() (it fired after 10 s, not 300). What follows is
    // Serial.flush() and esp_restart(). The flush is not bounded on the 7B: its
    // UART console's flush takes the Arduino UART mutex without a timeout
    // (uartFlushTxOnly, UART_MUTEX_LOCK = portMAX_DELAY) before the FIFO
    // drains on its own; it is ruled out only because nothing else held that
    // mutex - this task's own printf had taken and released it just before.
    // (The USB console of 5B/4" gives up after its TX timeout, here the full
    // 100 ms on both: consoleFlushFull(), UsbConsole.h.)
    // esp_restart()'s handlers run newest first, and in this firmware
    // they are esp_wifi_stop() (the WiFi default handlers), the PANIC_PATH_TEST
    // rsthang handler (returns unless armed) and esp_sync_timekeeping_timers()
    // - every caller of esp_register_shutdown_handler in the 7B image. Only
    // esp_wifi_stop() waits for another task - the WiFi task carries the stop
    // out - and its API documents no timeout for that. With the network side
    // starved, that is the one wait on this path with no end of its own; what
    // the WiFi task itself was stuck on is not known (the WiFi library is
    // binary-only), so
    // this is the likeliest cause, not a proven one: the bench test A1/A3
    // repeated with this build is what accepts it (expected: a reset - rst
    // 0x03 - right after the H line, not the backstop's rst 0x10 10 s
    // later). Skipping
    // the handlers loses nothing a reset needs: the station leaves without a
    // deauth, as after any panic, and the clock is set again by SNTP. The
    // backstop above stays for whatever else might hang here.
    vTaskSuspendAll();
    esp_restart_noos();
}

inline bool advanced(Watch &w, uint32_t value, uint32_t now) {
    if (value != w.last) { w.last = value; w.changedAt = now; }
    return now - w.changedAt < T_STALL_MS;
}

// The 1 s check, in the esp_timer task.
void guardTick(void *) {
    if (!s_pending) return;
    const uint32_t now = millis();
    if (!s_setupOk) {
        if (now - s_bootMs >= T_SETUP_MAX_MS) rollBack(1);   // H1
        return;
    }
    const uint32_t since = now - s_setupMs;
    const bool h2 = advanced(s_wLoop, s_loopCount, now);
    const bool h3 = advanced(s_wFlush, g_lvFlushCount, now);
    bool h4 = true;
    if (s_n2kTask) {
        // From the moment the task was created - also if it hangs before
        // N2kHandler::begin() got as far as installing the bus probe.
        h4 = advanced(s_wN2k, g_n2kLoopCount, now);
        // The controller down with nobody bringing it back. Bus-off and
        // recovery - also a recovery that keeps failing on a miswired bus -
        // are tolerated: a wiring fault is not a firmware fault (10.2 H4).
        uint32_t stuckAt;
        if (h4 && s_busStuckFn && s_busStuckFn(stuckAt) && now - stuckAt > T_BUS_DOWN_MAX_MS)
            h4 = false;
    }
    bool h5 = true;
    if (s_h5Required) {
        const bool proof = s_h5Mode == UpdNvs::H5_PROTOCOL ? (s_proofFn && s_proofFn())
                                                           : g_anyN2kFrame;
        h5 = s_selfTestsOk && proof;
    }
    // H2-H4: roll back once one has failed for 15 s in a row.
    const bool ok[3] = { h2, h3, h4 };
    for (int i = 0; i < 3; i++) {
        uint32_t &fs = s_failSince[i + 1];
        if (ok[i]) { fs = 0; continue; }
        if (!fs) fs = now ? now : 1;
        else if (now - fs >= T_FAIL_MS) { rollBack((uint8_t)(i + 2)); return; }
    }
    const uint32_t left = since < T_CAP_MS ? (T_CAP_MS - since + 999) / 1000 : 0;
    UpdUi::setVerify(true, (uint16_t)left, FW_VERSION);
    if (since >= T_EARLIEST_MS && h2 && h3 && h4 && h5 && markValid()) return;
    if (since >= T_CAP_MS) {
        // Not valid in time; H5 can only fail this way (detail 0: every
        // criterion held, but otadata could not be written).
        rollBack(!h2 ? 2 : !h3 ? 3 : !h4 ? 4 : !h5 ? 5 : 0);
    }
}

}  // namespace

// ---- boot ---------------------------------------------------------------------------------
void bootEarly() {
    s_bootMs = millis();
    s_running = esp_ota_get_running_partition();
    if (s_running && s_running->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
        s_running->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_1)
        s_slot = (uint8_t)(s_running->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0);
    const esp_app_desc_t *d = updAppDesc();
    if (d) memcpy(s_id, d->app_elf_sha256, 8);

    // 0. Anchor. With erased otadata (flashed without boot_app0.bin) the
    //    running image has no entry, and after the first OTA the old image
    //    would be no valid rollback target. One write fixes that for good.
    esp_err_t e = s_running ? esp_ota_get_state_partition(s_running, &s_state) : ESP_FAIL;
    if (e == ESP_ERR_NOT_FOUND && s_running) {
        if (esp_ota_set_boot_partition(s_running) == ESP_OK) {
            esp_ota_mark_app_valid_cancel_rollback();
            s_anchored = true;
            note("[ota] anchored");
        }
        e = esp_ota_get_state_partition(s_running, &s_state);
    }
    // 1. The running state.
    s_haveState = e == ESP_OK;
    s_pending = s_haveState && s_state == ESP_OTA_IMG_PENDING_VERIFY;

    // 2. Crash-loop guard - only a validated image is in its window.
    if (!s_pending) crashLoopCheck();
    // 3. Rollback report for the old firmware.
    if (!s_pending) rollbackDetect();

    // 5.3.6 step 4: an approval belongs to the first boot of the image it
    // names. Taken into RAM there (usable after mark-valid), deleted from NVS
    // at every boot - so it can never outlive that boot or a rollback.
    UpdNvs::PreWeb pw;
    if (UpdNvs::loadPreWeb(pw)) {
        if (s_pending && memcmp(pw.imageId, s_id, 8) == 0) { s_preweb = pw; s_prewebLoaded = true; }
        UpdNvs::clearPreWeb();
    }

    // 4. The verify window.
    if (s_pending) {
        UpdNvs::H5 h5;
        if (UpdNvs::loadH5(h5) && memcmp(h5.imageId, s_id, 8) == 0) {
            s_h5Required = true;
            s_h5Mode = h5.mode;
            // PROTOCOL expects the sender's INFO polls, and those only follow
            // the restart the countdown or "Restart now" make - esp_restart(),
            // ESP_RST_SW. Any other way into this image (a power cycle while
            // the countdown waited for an alarm, a 4-inch that had to be
            // power-cycled out of download mode) finds no sender polling:
            // then any N2K message is the proof, as after "Later".
            if (s_h5Mode == UpdNvs::H5_PROTOCOL && esp_reset_reason() != ESP_RST_SW) {
                s_h5Mode = UpdNvs::H5_ANY_FRAME;
                note("[ota] not restarted by the update: H5 accepts any N2K message");
            }
        }
        // Validated once, here in the loop task's 8 KB stack, instead of in
        // the esp_timer task at the moment of a rollback (H1 included): the
        // other slot is not written during the window (esp_ota_begin refuses).
        s_rbPossible = esp_ota_check_rollback_is_possible();
        backstopArm(BACKSTOP_MS);
        UpdateGate::setVerifyWindow(true, s_bootMs + T_SETUP_MAX_MS + T_CAP_MS);
        esp_timer_create_args_t a;
        memset(&a, 0, sizeof(a));
        a.callback = &guardTick;
        a.dispatch_method = ESP_TIMER_TASK;
        a.name = "otaguard";
        if (esp_timer_create(&a, &s_timer) == ESP_OK) esp_timer_start_periodic(s_timer, 1000000ULL);
        else note("[ota] guard timer could not be created - the bootloader decides");
    }
}

void bootReport() {
    if (s_report[0]) Serial.print(s_report);
    if (s_pending)
        Serial.printf("[ota] new image PENDING_VERIFY - health check running%s\n",
                      s_h5Required ? (s_h5Mode == UpdNvs::H5_PROTOCOL
                                          ? " (H5: N2K update protocol)"
                                          : " (H5: any N2K message)")
                                   : "");
}

void setupDone() {
    // Always log WHICH slot is running. Without this an OTA test is
    // unfalsifiable: both slots hold the same firmware, so a silent rollback
    // looks exactly like a successful update.
    Serial.printf("[ota] running from '%s' @ 0x%06X, state=%d\n",
                  s_running ? s_running->label : "?",
                  s_running ? (unsigned)s_running->address : 0u,
                  s_haveState ? (int)s_state : -1);
    s_setupMs = millis();
    if (s_pending) {
        UpdateGate::setVerifyWindow(true, s_setupMs + T_CAP_MS);
        UpdUi::setVerify(true, (uint16_t)(T_CAP_MS / 1000), FW_VERSION);
    }
    s_setupOk = true;
    if (s_noticePending) {
        UpdUi::Notice n;
        memset(&n, 0, sizeof(n));
        n.cause = s_rb.cause;
        n.detail = s_rb.detail;
        memcpy(n.failedVersion, s_rb.version, 16);
        strncpy(n.runningVersion, FW_VERSION, 16);
        UpdUi::postRollbackNotice(n);
        s_noticePending = false;
    }
}

void loopTick() {
#if defined(OTA_TEST_NEVER_HEALTHY)
    // Bench test 10.7/2: an image that stops its loop counter 30 s after boot.
    if (millis() > 30000) return;
#endif
    s_loopCount = s_loopCount + 1;   // H2 (no ++ on a volatile: C++20)
    const uint32_t now = millis();
#if defined(OTA_TEST_HANG)
    // Bench test 10.7/5: a total hang 20 s after boot, both cores, interrupts
    // masked - on the 7B/5B with WiFi on only the RTC watchdog can end it (at
    // 300 s). Where the IDF task watchdog is still live (the 4" with WiFi on,
    // any board with WiFi off) its hardware stage ends it first, after ~15 s.
    if (now > 20000) {
        xTaskCreatePinnedToCore([](void *) { portDISABLE_INTERRUPTS(); for (;;) {} },
                                "hang", 1024, nullptr, 24, nullptr, 0);
        portDISABLE_INTERRUPTS();
        for (;;) {}
    }
#endif
#if defined(OTA_TEST_PANIC_AT)
    // Bench test 10.7/6: marked valid at 60 s, a panic at OTA_TEST_PANIC_AT s.
    if (s_setupOk && now - s_setupMs > (uint32_t)(OTA_TEST_PANIC_AT) * 1000UL) abort();
#endif
    // 10.6: cumulative uptime of the crash-loop window, written once a minute.
    // Signed differences: `now` was read above, and markValid() (esp_timer
    // task) may have set the window's start a moment later - an unsigned
    // difference would wrap and end the window at once.
    if (s_clgActive && !s_pending && (int32_t)(now - s_clgWriteMs) >= (int32_t)CLG_WRITE_MS) {
        s_clgWriteMs = now;
        const int32_t up = (int32_t)(now - s_clgFromMs);
        const uint32_t secs = s_clgBaseS + (up > 0 ? (uint32_t)up / 1000 : 0);
        if (secs >= CLG_WINDOW_S) {
            UpdNvs::clearClg();
            s_clgActive = false;
            s_crash.magic = 0;
            Serial.println("[ota] crash-loop window over (30 min)");
        } else {
            UpdNvs::Clg clg;
            memcpy(clg.imageId, s_id, 8);
            clg.seconds = secs;
            memcpy(clg.otherId, s_clgOther, 8);
            UpdNvs::storeClg(clg);
        }
    }
}

void setN2kProbes(bool (*receptionProof)(), bool (*busStuckSince)(uint32_t &ms)) {
    s_proofFn = receptionProof;
    s_busStuckFn = busStuckSince;
}

void setN2kTaskStarted() { s_n2kTask = true; }

void setSelfTestsPassed(bool ok) { s_selfTestsOk = ok; }

// ---- state ------------------------------------------------------------------------------
uint8_t otaState() {
    if (s_pending) return n2kupd::OTA_PENDING_VERIFY;
    return s_haveState || s_valid ? (uint8_t)n2kupd::OTA_VALID : (uint8_t)n2kupd::OTA_NA;
}
bool pendingVerify() { return s_pending; }

uint16_t verifySecondsLeft() {
    if (!s_pending) return 0xFFFF;
    if (!s_setupOk) return (uint16_t)(T_CAP_MS / 1000);
    const uint32_t since = millis() - s_setupMs;
    return since >= T_CAP_MS ? 0 : (uint16_t)((T_CAP_MS - since + 999) / 1000);
}

uint8_t runningSlot() { return s_slot; }
const uint8_t *runningImageId() { return s_id; }
bool setupHasFinished() { return s_setupOk; }

bool rollbackInfo(uint8_t id[8], uint8_t version[16], uint8_t &cause, uint8_t &detail) {
    if (!s_rbValid) return false;
    memcpy(id, s_rb.imageId, 8);
    memcpy(version, s_rb.version, 16);
    cause = s_rb.cause;
    detail = s_rb.detail;
    return true;
}

// The report and a cause record describe the image that is about to be
// overwritten, so they go too. That keeps a later rollback of the same build,
// installed again, from reading as the old one (rollbackDetect()). Nothing for
// the running image is lost: its own cause record is only ever written right
// before its restart, and no update can start then. Called by
// OtaWriter::begin(), `marked` = the slot's dirty mark was stored. The report
// in RAM ends either way - /api/info (lastInvalidVersion, rollbackCause) and
// INFO (status flag 3) must not name the old image while the upload overwrites
// the slot. The NVS records are deleted only with the mark (see
// OtaWriter::begin() for why): the next boot then still reads the old report
// from them, should the slot still hold that image.
void otherSlotOverwritten(bool marked) {
    s_rbValid = false;
    if (!marked) return;
    UpdNvs::clearRbInfo();
    UpdNvs::clearCause();
}

void rollbackNoticeSeen() {
    UpdNvs::RbInfo rb;
    if (UpdNvs::loadRbInfo(rb)) UpdNvs::storeRbSeen(rb.imageId);
}

// ---- companion web UI (5.3.6) --------------------------------------------------------------
bool companionWaiting() {
    return s_prewebLoaded && s_valid && millis() - s_validAtMs < COMPANION_MS;
}

bool companionApproved(const uint8_t sha[32], uint32_t size, const uint8_t version16[16]) {
    return companionWaiting() && s_preweb.size == size &&
           memcmp(s_preweb.sha, sha, 32) == 0 && memcmp(s_preweb.version, version16, 16) == 0;
}

void companionConsumed() { s_prewebLoaded = false; }

}  // namespace OtaGuard

extern "C" void otaGuardNoteAssert(void) {
    if (OtaGuard::s_crash.magic == OtaGuard::CRASH_MAGIC) OtaGuard::s_crash.assertFlag = 1;
}

#else  // SIMULATOR
#include "OtaGuard.h"
extern "C" void otaGuardNoteAssert(void) {}
#endif
