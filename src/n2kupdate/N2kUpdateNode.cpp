// ============================================================================
// N2kUpdateNode.cpp - see N2kUpdateNode.h.
// ============================================================================
#ifndef SIMULATOR
#include "N2kUpdateNode.h"
#include "N2kUpdateCore.h"
#include "FwDesc.h"
#include "OtaGuard.h"
#include "OtaWriter.h"
#include "RomInflater.h"
#include "UpdCrypto.h"
#include "UpdNvs.h"
#include "UpdRegions.h"
#include "UpdUi.h"
#include "UpdateGate.h"
#include "WebUiWriter.h"
#include "../BoardConfig.h"
#include "../Version.h"
#include "../config/Config.h"
#include "../display/DisplayManager.h"
#include "../LoopWatchdog.h"
#include "../UsbConsole.h"   // consoleFlushFull() before the restart
#include <Arduino.h>
#include <LittleFS.h>
#include <NMEA2000.h>
#include <NMEA2000_esp32.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <new>
#include <string.h>

extern tNMEA2000 &NMEA2000;

using namespace n2kupd;

namespace N2kUpdateNode {
namespace {

// ---- the interfaces of N2kUpdateCore.h -----------------------------------------------

class Clock : public IUpdClock {
public:
    uint32_t millis() override { return ::millis(); }
};

class Link : public IUpdLink {
public:
    uint8_t ownAddress() override { return NMEA2000.GetN2kSource(); }
    bool send(uint8_t dst, uint8_t prio, const uint8_t *payload, size_t len) override {
        if (len > MAX_PAYLOAD) return false;
        tN2kMsg m;
        // Init(): priority, PGN, destination, length 0, multi-packet flag off.
        // The library frames 126720 as fast packet even below 9 bytes (2.3
        // rule 1) and fills in our claimed source address.
        m.Init(prio, PGN, 0, dst);
        m.AddBuf(payload, len);
        return NMEA2000.SendMsg(m);
    }
    bool txIdle() override { return static_cast<tNMEA2000_esp32 &>(NMEA2000).TxIdle(); }
};

class Host : public IUpdHost {
public:
    bool setupDone() override       { return OtaGuard::setupHasFinished(); }
    bool selfTestsPassed() override { return s_selfOk; }
    bool regionsReserved() override { return UpdRegions::reserved(); }
    bool acceptUpdates() override   { return appConfig.cfg.n2kAcceptUpdates; }
    bool wifiOtaActive() override   { return UpdateGate::wifiOtaActive(); }
    bool runningPendingVerify() override { return OtaGuard::pendingVerify(); }
    bool restartPending() override  { return UpdateGate::restartPending(); }
    bool alarmActive() override     { return DisplayManager::alarmActive(); }
    bool restartCountdownOpen() override { return UpdUi::countdownOpen(); }
    uint32_t fsFreeBytes() override {
        const size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
        return used < total ? (uint32_t)(total - used) : 0;
    }
    bool companionApproved(const uint8_t sha[32], uint32_t size, const uint8_t version[16]) override {
        return OtaGuard::companionApproved(sha, size, version);
    }
    void fillInfo(InfoMsg &m) override {
        m.otaState = OtaGuard::otaState();
        m.verifySecondsLeft = OtaGuard::verifySecondsLeft();
        m.runningSlot = OtaGuard::runningSlot();
        uint8_t f = 0;
        if (OtaGuard::rollbackInfo(m.lastInvalidImageId, m.lastInvalidVersion,
                                   m.rollbackCause, m.rollbackDetail))
            f |= SF_ROLLED_BACK;
        if (OtaGuard::companionWaiting()) f |= SF_COMPANION_WAITING;
        m.statusFlags = f;
        putUtf8Field(m.label, appConfig.cfg.bootName);
        memcpy(m.runningImageId, OtaGuard::runningImageId(), 8);
        UpdRegions::webUiIdentity(m.webUiId, m.webUiSize, m.webUiVersion);
        m.uptimeS = ::millis() / 1000;
    }
    uint32_t acquireWorkArea(WorkArea &out) override { return UpdRegions::acquireWork(out); }
    void releaseWorkArea() override { UpdRegions::releaseWork(); }
    void sessionStateChanged(uint8_t state) override {
        UpdateGate::setSessionState(state);
        // RECEIVING and PROCESSING alternate once per window - 180 times in
        // a firmware transfer - so only the other transitions are logged.
        if (state != STATE_RECEIVING && state != STATE_PROCESSING) {
            static const char *const NAMES[] = { "idle", "confirming", "preparing",
                "receiving", "processing", "verifying", "ready", "applying", "done", "failed" };
            Serial.printf("[n2kupd] session %s\n", state < 10 ? NAMES[state] : "?");
        }
    }
    static bool s_selfOk;
};
bool Host::s_selfOk = false;

Clock        s_clock;
Link         s_link;
Host         s_host;
MbedSha256   s_sha;                       // internal RAM: the hardware SHA DMAs from it
RomInflater  s_inflater;
OtaWriter    s_firmware(UpdateGate::OWNER_N2K);
// The web UI reload in APPLY borrows the receiver's SHA object (done by then).
WebUiWriter  s_webUi(UpdateGate::OWNER_N2K, &s_sha);

Receiver    *s_rx       = nullptr;        // placement-new into the PSRAM block
static_assert(sizeof(Receiver) + alignof(Receiver) <=
                  UpdRegions::ARENA_BYTES - 2 * UpdRegions::REGION_BYTES,
              "the receiver does not fit its PSRAM block");
bool         s_running  = false;          // begin() wired it to the bus
bool         s_canSend  = false;
volatile bool s_listenProof = false;      // H5 PROTOCOL in listen-only mode
TaskHandle_t s_task     = nullptr;
uint32_t     s_lastSnapMs = 0;

bool receptionProof() { return s_running ? s_rx->receptionProof() : (bool)s_listenProof; }

bool busStuckSince(uint32_t &ms) {
    return static_cast<tNMEA2000_esp32 &>(NMEA2000).StuckSinceMs(ms);
}

// Listen-only: the receiver stays inert (it could not answer), but an image
// installed over NMEA 2000 still needs its reception proof (H5). The same
// criteria as the receiver's: a sender->display type of this protocol, v1,
// the minimum length, addressed to us or to 255 with a matching filter.
void listenOnlyProof(uint8_t dst, const uint8_t *p, size_t len) {
    const uint8_t type = msgType(p, len);
    if (!type || !isSenderToDisplay(type) || len < minLength(type)) return;
    const uint8_t own = NMEA2000.GetN2kSource();
    if (dst != own && !(type == MSG_QUERY && dst == ADDR_BROADCAST)) return;
    if (type != MSG_DATA && p[3] != PROTO_VERSION) return;
    if (type == MSG_QUERY) {
        uint8_t mac[6];
        esp_efuse_mac_get_default(mac);
        if (!isAllFF(p + 6, 6) && memcmp(p + 6, mac, 6) != 0) return;
    }
    s_listenProof = true;
}

// ---- UI -> device hooks (display task) --------------------------------------------------
// The sender polls INFO for 5 minutes after APPLY (5.7). A restart later than
// this (the countdown waited for an alarm) is met by nobody polling; minus
// the boot time and one poll interval.
constexpr uint32_t T_LATE_RESTART_MS = 240000;

// The H5 marker from "the sender's polls" (PROTOCOL) to "any N2K message".
void relaxH5() {
    UpdNvs::H5 h5;
    if (UpdNvs::loadH5(h5) && h5.mode != UpdNvs::H5_ANY_FRAME) {
        h5.mode = UpdNvs::H5_ANY_FRAME;
        UpdNvs::storeH5(h5);
    }
}

void hookRestartNow() {
    uint32_t at;
    if (OtaWriter::activatedAt(at) && ::millis() - at > T_LATE_RESTART_MS) {
        relaxH5();
        Serial.println("[n2kupd] restart long after the activation - H5 accepts any N2K message");
    }
    Serial.println("[n2kupd] restarting to activate the update");
    consoleFlushFull();
    delay(100);
    LoopWatchdog::phase(LoopWatchdog::PH_RESTART);   // a hang in there: the loop watch
    ESP.restart();
}

void hookLater() {
    // 7.4 "Later": the image becomes active at the next restart or power
    // cycle, and then no sender polls - so H5 waits for any N2K message.
    relaxH5();
    Serial.println("[n2kupd] restart postponed - the update becomes active at the next restart");
}

void hookNoticeSeen() { OtaGuard::rollbackNoticeSeen(); }

}  // namespace

// ---- boot ------------------------------------------------------------------------------------
void bootPrepare() {
    UpdUi::Hooks hooks = { hookRestartNow, hookLater, hookNoticeSeen };
    UpdUi::setHooks(hooks);

    void *mem = UpdRegions::receiverStorage(sizeof(Receiver), alignof(Receiver));
    if (mem) s_rx = new (mem) Receiver();
    else Serial.println("[n2kupd] no PSRAM block for the receiver - updates over NMEA 2000 off");

    // Self-test 1 (8.1): the descriptor sits exactly 0x100 behind the app
    // description, i.e. at image offset 0x120 - otherwise the linker placed
    // it elsewhere and no sender would find it.
    int fail = 0;
    const esp_app_desc_t *app = updAppDesc();
    if (!app || (uintptr_t)&np_fw_desc - (uintptr_t)app != 0x100) fail = 1;
    // Self-tests 2-4 through the very objects the receiver uses; test 4 needs
    // the work region (dictionary and decompressor state in PSRAM). Without
    // the regions it cannot run, and that is not an inflater fault: OFFERs
    // then get OUT_OF_MEMORY (9.1) rather than INTERNAL_ERROR, so test 4 is
    // skipped and the missing regions are reported as what they are.
    const bool regions = UpdRegions::reserved();
    if (!fail) {
        WorkArea wa;
        const uint32_t wait = regions ? UpdRegions::acquireWork(wa) : 0;
        if (!regions) {
            fail = runSelfTests(updCrc32, s_sha, nullptr, nullptr, nullptr, UpdRegions::bounce());
        } else if (wait || !wa.dict) {
            fail = 4;
        } else {
            fail = runSelfTests(updCrc32, s_sha, &s_inflater, wa.inflaterState, wa.dict,
                                UpdRegions::bounce());
            UpdRegions::releaseWork();
        }
    }
    Host::s_selfOk = fail == 0;
    // H5 (10.2) asks whether this image can take the next update over NMEA
    // 2000: not without its regions either.
    OtaGuard::setSelfTestsPassed(fail == 0 && regions);
    if (fail) Serial.printf("[n2kupd] selftest FAIL %d\n", fail);
    else if (!regions) Serial.println("[n2kupd] selftest ok (4 skipped: no update regions)");
    else Serial.println("[n2kupd] selftest ok");
}

void begin(bool canSend) {
    s_task = xTaskGetCurrentTaskHandle();
    s_canSend = canSend;
    OtaGuard::setN2kProbes(receptionProof, busStuckSince);
    if (!canSend) {
        Serial.println("[n2kupd] listen-only: updates over NMEA 2000 are not possible "
                       "(the display cannot answer)");
        return;
    }
    if (!s_rx) return;

    Identity id;
    memset(&id, 0, sizeof(id));
    id.boardId = NP_BOARD_ID;
    id.panelW = LCD_WIDTH;
    id.panelH = LCD_HEIGHT;
    esp_efuse_mac_get_default(id.uniqueId);        // transmission order = display order
    putVersionField(id.fwVersion, FW_VERSION);

    n2kupd::Config cfg = defaultConfig();   // not ::Config, the settings class
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
    cfg.maxFirmwareSize = next ? (uint32_t)next->size - 4096 : 0;
    cfg.maxWebUiSize = UpdRegions::REGION_BYTES;
    cfg.flashGapMs = UPD_FLASH_GAP_MS;

    Env env;
    memset(&env, 0, sizeof(env));
    env.clock = &s_clock;
    env.link = &s_link;
    env.host = &s_host;
    env.ui = &UpdUi::receiverUi();
    env.sha = &s_sha;
    env.inflater = &s_inflater;
    env.firmware = next ? &s_firmware : nullptr;
    env.webUi = &s_webUi;
    env.crc32 = updCrc32;
    env.bounce = UpdRegions::bounce();
    s_running = s_rx->begin(env, id, cfg);
    Serial.printf("[n2kupd] receiver %s, capabilities 0x%02X, unique id %02X:%02X:%02X:%02X:%02X:%02X\n",
                  s_running ? "ready" : "NOT started", (unsigned)s_rx->capabilities(),
                  id.uniqueId[0], id.uniqueId[1], id.uniqueId[2],
                  id.uniqueId[3], id.uniqueId[4], id.uniqueId[5]);
}

// ---- runtime (N2K task) ----------------------------------------------------------------------
void onMessage(const tN2kMsg &msg) {
#if defined(OTA_TEST_N2K_DEAF)
    // Bench test H22: an image that never hears the protocol must roll back.
    return;
#endif
    if (msg.DataLen <= 0) return;
    if (s_running) s_rx->onMessage(msg.Source, msg.Destination, msg.Data, (size_t)msg.DataLen);
    else listenOnlyProof(msg.Destination, msg.Data, (size_t)msg.DataLen);
}

void tick() {
    if (!s_running) return;
    UpdUi::UserCmd cmd;
    uint32_t token;
    if (UpdUi::takeUserCommand(cmd, token)) {
        switch (cmd) {
            case UpdUi::CMD_ACCEPT:  s_rx->userAccept(token);  break;
            case UpdUi::CMD_DECLINE: s_rx->userDecline(token); break;
            case UpdUi::CMD_CANCEL:  s_rx->userCancel(token);  break;
            default: break;
        }
    }
    if (UpdUi::takeIdentifyClosed()) s_rx->userIdentifyClosed();
    s_rx->tick();
    // Progress for the banner (7.3): a fresh snapshot four times a second.
    if (s_rx->state() != STATE_IDLE) {
        const uint32_t now = ::millis();
        if (now - s_lastSnapMs >= 250) {
            s_lastSnapMs = now;
            UpdUi::postSnapshot(s_rx->snapshot());
        }
    }
}

uint8_t sessionState()    { return s_rx ? s_rx->state() : (uint8_t)STATE_IDLE; }
bool    sessionActive()   { return s_rx && s_rx->sessionActive(); }
bool    selfTestsPassed() { return Host::s_selfOk; }
uint8_t capabilities()    { return s_running ? s_rx->capabilities() : 0; }

uint32_t n2kStackHighWater() {
    return s_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0;
}

}  // namespace N2kUpdateNode
#endif  // SIMULATOR
