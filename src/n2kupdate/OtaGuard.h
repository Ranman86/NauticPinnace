#pragma once
// ============================================================================
// OtaGuard - rollback that actually works (docs/N2K-UPDATE.md section 10).
//
// All three boards' bootloaders have rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_
// ENABLE): a freshly installed image boots PENDING_VERIFY, and if it is not
// confirmed before the next reset, the bootloader marks it ABORTED and boots
// the previous one. What defeated this until now was Arduino's initArduino():
// it confirms every PENDING_VERIFY image BEFORE setup() runs, whenever the
// weak verifyRollbackLater() returns false - which is the default. This
// module defines it as true (with C linkage) and confirms the image itself,
// only once it has shown that it works:
//
//   H1  setup() finished within 90 s
//   H2  loop() keeps running            (counter advanced within 5 s)
//   H3  LVGL keeps flushing             (counter advanced within 5 s; the
//                                         1 Hz verify banner guarantees one)
//   H4  the N2K task keeps running and the CAN controller is not left down
//       (bus-off and its recovery, even one that keeps failing on a miswired
//       bus, are a wiring fault, not a firmware fault)
//   H5  an image installed over NMEA 2000 has received the protocol (APPLY
//       mode 0: the sender's INFO polls) or any N2K message (modes 1 and 2,
//       "Later"), and the receiver's self-tests passed
//
// Marked valid at the first check at least 60 s after setupDone() at which
// H2-H5 hold. Rolled back at once if H1 fails or any of H2-H4 has failed
// continuously for 15 s, and in any case if not valid 180 s after
// setupDone(). The check runs in an esp_timer, so it depends neither on the
// loop nor on LVGL; the RTC watchdog (300 s, never fed; programmed as a system
// reset with an RTC reset 2 s later - measured, the first stage itself ends
// as the RTC reset, rst 0x10) backs it up against a hang that stops esp_timer
// too. It is programmed through LoopWatchdog, which owns the timer, and at
// mark-valid it is handed over to the loop watch instead of being switched off.
//
// It also reports the last rollback to the old firmware (cause and version,
// through NVS - 10.5), guards against crash loops in the first 30 minutes of
// a new image (10.6), and holds the companion web UI approval (5.3.6).
// ============================================================================
#include <stdint.h>

// LV_ASSERT_HANDLER (lv_conf.h) calls this before esp_restart(): an assert
// counts as a crash for the crash-loop guard (10.6). C linkage, because LVGL
// is C.
#ifdef __cplusplus
extern "C" {
#endif
void otaGuardNoteAssert(void);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
namespace OtaGuard {

// Health counters, incremented at their anchors (10.2) and read by the
// esp_timer task. Only ever written by their one task, hence no lock; the
// helpers spell out the increment because C++20 deprecates ++ on a volatile.
extern volatile uint32_t g_lvFlushCount;    // lvgl_flush_cb, both display backends
extern volatile uint32_t g_n2kLoopCount;    // the N2K task's loop
extern volatile bool     g_anyN2kFrame;     // any N2K message parsed since boot (H5)
inline void countFlush()   { g_lvFlushCount = g_lvFlushCount + 1; }
inline void countN2kLoop() { g_n2kLoopCount = g_n2kLoopCount + 1; }

// ---- boot ---------------------------------------------------------------------------
void bootEarly();        // FIRST line of setup(), before the 8 s delay
void bootReport();       // once Serial is up: what bootEarly() found
void setupDone();        // the end of setup(): log line, verify window starts
void loopTick();         // every loop() pass (H2, crash-loop bookkeeping)

// main.cpp, next to creating the N2K task: from here on H4 watches the
// task's loop counter. Booted in demo mode there is no N2K task, H4 does not
// apply and H5 cannot be met.
void setN2kTaskStarted();
// The N2K side, installed by N2kUpdateNode::begin() in the N2K task.
// `busStuckSince`: the CAN controller is down and nothing brings it back
// (stopped, not in a bus-off recovery) - H4 fails after 20 s of that.
void setN2kProbes(bool (*receptionProof)(), bool (*busStuckSince)(uint32_t &ms));
void setSelfTestsPassed(bool ok);

// ---- state (INFO, /api/info) ------------------------------------------------------------
uint8_t        otaState();          // 4.6: 0 n/a, 1 VALID, 2 PENDING_VERIFY
bool           pendingVerify();
uint16_t       verifySecondsLeft(); // 0xFFFF = not verifying
uint8_t        runningSlot();       // 0 = app0, 1 = app1, 0xFF = unknown
const uint8_t *runningImageId();    // 8 bytes of esp_app_desc_t.app_elf_sha256
bool           setupHasFinished();
// The last rollback while the other slot still holds the rolled-back image
// (INFO flag 3, bytes 47, 87, 95, 96).
bool rollbackInfo(uint8_t id[8], uint8_t version[16], uint8_t &cause, uint8_t &detail);
// The other slot is about to be overwritten by a new update: the rollback
// report no longer describes what is in it. It ends at once (rollbackInfo()
// false); NVS rbinfo and cause are deleted only when `marked` - the slot's
// dirty mark was stored (OtaWriter::begin()).
void otherSlotOverwritten(bool marked);
// The rollback notice (7.5) was acknowledged: NVS n2kupd/rbseen, so it is
// shown once per rollback.
void rollbackNoticeSeen();

// ---- companion web UI (5.3.6) ------------------------------------------------------------
bool companionApproved(const uint8_t sha[32], uint32_t size, const uint8_t version16[16]);
bool companionWaiting();            // INFO status flag 5
void companionConsumed();           // the approved web UI has been applied

}  // namespace OtaGuard
#endif
