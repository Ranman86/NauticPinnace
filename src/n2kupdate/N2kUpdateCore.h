#pragma once
// ============================================================================
// N2kUpdateCore - the display's receiver for "updates over NMEA 2000"
// (docs/N2K-UPDATE.md, protocol v1 rev 2): session state machine (5.x),
// windows and credit, per-window stream CRC, write-time and read-back
// SHA-256, optional deflate input, image identity checks (8.x) and the write
// rules of section 9.
//
// PLATFORM-FREE. No Arduino, IDF, LVGL, FreeRTOS or NMEA2000 include. The
// device glue implements the small interfaces below; the host tests implement
// fakes. Nothing here allocates: every buffer comes from the caller, and the
// receiver object itself is a fixed-size record (sizeof(Receiver)).
//
// THREADING. Every entry point runs in ONE task, the N2K task on the device
// (9.1): onMessage() from the PGN 126720 handler, tick() from the N2K loop in
// both branches (bus and demo), the user*() calls from the UI mailbox drain.
// Interfaces are called back from that same task. IUpdUi implementations post
// to the display task; they must not touch LVGL directly.
//
// FLASH WORK. tick() does at most ONE unit of flash work and returns (9.1):
//   - copy or decode image bytes up to the next 4 KB boundary of the image
//     offset and write them in pieces of at most 256 bytes;
//   - read back 4 KB in pieces of 256 bytes and hash them;
//   - one monolithic step (target begin / finish / apply).
// A unit only starts while the sender holds no credit (PREPARING,
// PROCESSING, VERIFYING, APPLYING) and the link reports its transmit path idle,
// waiting for that at most 250 ms.
//
// THE BOUNCE RULE (9.1). Every pointer handed to IUpdTarget::write/readBack
// and to ISha256::update is the caller's 256-byte bounce buffer. The window
// buffer and the deflate dictionary (PSRAM on the device) are only ever read
// by the CPU (memcpy, CRC, inflater).
// ============================================================================
#include "N2kUpdateProto.h"

namespace n2kupd {

// ---- interfaces ----------------------------------------------------------------------

// Monotonic milliseconds (millis()); wraps after 49 days, all arithmetic is
// unsigned and wrap-safe.
class IUpdClock {
public:
    virtual ~IUpdClock() {}
    virtual uint32_t millis() = 0;
};

// The NMEA 2000 side. send() queues ONE complete message as PGN 126720 to
// `dst` with priority `prio`; the glue fast-packet frames it (every message,
// even short ones, 2.3). Returns false if the message could not be queued
// (it is then simply lost; the protocol's timers recover).
class IUpdLink {
public:
    virtual ~IUpdLink() {}
    virtual uint8_t ownAddress() = 0;                     // GetN2kSource()
    virtual bool send(uint8_t dst, uint8_t prio, const uint8_t *payload, size_t len) = 0;
    // Library send ring empty and TWAI msgs_to_tx == 0 (9.1).
    virtual bool txIdle() = 0;
};

// SHA-256 (8.4). Input is always the bounce buffer.
class ISha256 {
public:
    virtual ~ISha256() {}
    virtual void init() = 0;
    virtual void update(const uint8_t *data, size_t n) = 0;
    virtual void finish(uint8_t out[32]) = 0;
};

// Raw-deflate decoder with the contract of tinfl_decompress() as the ROM has
// it (9.3): no zlib header, wrapping output into the 32 KB dictionary.
//   in/inBytes:   input; on return *inBytes = bytes consumed.
//   dict/dictPos: the dictionary and the write position in it; the output
//                 space is ALWAYS [dictPos, DICT_BYTES) - tinfl rejects any
//                 other size for a wrapping buffer.
//   *outBytes:    on entry DICT_BYTES - dictPos, on return bytes produced.
//   hasMoreInput: TINFL_FLAG_HAS_MORE_INPUT.
// Returns the tinfl status: 0 DONE, 1 NEEDS_MORE_INPUT, 2 HAS_MORE_OUTPUT,
// negative = error. The receiver never writes into the dictionary.
class IInflater {
public:
    virtual ~IInflater() {}
    // Resets the decompressor; `state` is WorkArea::inflaterState.
    virtual bool init(void *state) = 0;
    virtual int decompress(const uint8_t *in, size_t *inBytes, uint8_t *dict,
                           size_t dictPos, size_t *outBytes, bool hasMoreInput) = 0;
};
static constexpr int INFL_DONE             = 0;
static constexpr int INFL_NEEDS_MORE_INPUT = 1;
static constexpr int INFL_HAS_MORE_OUTPUT  = 2;
static constexpr int INFL_CANNOT_PROGRESS  = -4;  // newer miniz: input exhausted without HAS_MORE_INPUT

// What APPLY hands the target (5.6, 5.3.6, 10.2 H5 marker).
struct ApplyInfo {
    uint8_t  target;            // TARGET_FIRMWARE / TARGET_WEBUI
    uint8_t  mode;              // APPLY_NOW / APPLY_NEXT_RESTART / APPLY_NOW_NO_POLL
    uint8_t  imageId[8];        // firmware: read back from flash at 0xB0
    uint8_t  version[16];       // the OFFER's version field
    uint8_t  sha256[32];        // read-back SHA-256 = the OFFER's
    uint32_t imageSize;
    uint8_t  companionSha[32];  // firmware: the approved web UI, all zero = none
    uint32_t companionSize;
    bool     viaCompanion;      // web UI: accepted through a companion approval
};

enum ApplyStatus : uint8_t {
    APPLY_ST_DONE  = 0,   // finished; *detail is set (web UI: 0 served now, 1 next restart)
    APPLY_ST_AGAIN = 1,   // more work (e.g. cache reload, 4 KB per call): call again next tick
    APPLY_ST_BUSY  = 2,   // transient (rename EBUSY): retried every 100 ms for up to 10 s
    APPLY_ST_ERROR = 3,   // RESULT(APPLY_FAILED)
};

// Where the image goes (9.2 OtaWriter, 9.4 WebUiWriter). Writes are strictly
// sequential; `offset` is the image offset and equals the bytes written so
// far. The first write into a new 4 KB sector may erase it (esp_ota_write);
// the receiver guarantees that one unit enters at most one new sector.
// Every call returns RSN_OK or a reason code (4.8).
class IUpdTarget {
public:
    virtual ~IUpdTarget() {}
    // Firmware: esp_ota_begin(SEQUENTIAL). Web UI: delete a stale .new, open it.
    virtual uint8_t begin(uint32_t imageSize) = 0;
    virtual uint8_t write(uint32_t offset, const uint8_t *bounce, size_t n) = 0;
    // Firmware: esp_ota_end (IMAGE_INVALID on failure). Web UI: close (FLASH_ERROR).
    virtual uint8_t finish() = 0;
    // Reads back written bytes into the bounce buffer (FLASH_ERROR on failure).
    virtual uint8_t readBack(uint32_t offset, uint8_t *bounce, size_t n) = 0;
    // Firmware: set boot partition + H5 marker + companion approval, one call.
    // Web UI: rename (BUSY while a name is open), then reload the cache.
    virtual ApplyStatus apply(const ApplyInfo &info, uint8_t *detail) = 0;
    // esp_ota_abort() or deleting the temporary file. Must tolerate any state,
    // including after finish().
    virtual void abort() = 0;
};

// The two PSRAM pointers of the work region (9.1), handed out per session.
struct WorkArea {
    uint8_t *window;            // WINDOW_BYTES (13,824)
    uint8_t *dict;              // DICT_BYTES (32,768); may be null for web UI sessions
    void    *inflaterState;     // passed to IInflater::init()
};

// Everything about the device the receiver must ask (5.3.1, 4.5, 9.1, 11.1
// UpdateGate). Called from the N2K task; implementations read atomics.
class IUpdHost {
public:
    virtual ~IUpdHost() {}
    virtual bool setupDone() = 0;              // setup() has finished
    virtual bool selfTestsPassed() = 0;        // 8.1 boot self-tests
    virtual bool regionsReserved() = 0;        // UpdRegions::reserve() succeeded
    virtual bool acceptUpdates() = 0;          // nmea2000.accept_updates, live
    virtual bool wifiOtaActive() = 0;          // UpdateGate: a WiFi OTA owns the writer
    virtual bool runningPendingVerify() = 0;   // OtaGuard is still deciding
    virtual bool restartPending() = 0;         // a new firmware is activated, not running
    virtual bool alarmActive() = 0;            // any alarm (anchor drag, ...)
    virtual bool restartCountdownOpen() = 0;   // 7.4 dialog is on screen
    virtual uint32_t fsFreeBytes() = 0;        // free LittleFS bytes
    // 5.3.6: an approved companion for exactly this web UI is usable now.
    virtual bool companionApproved(const uint8_t sha[32], uint32_t size,
                                   const uint8_t version[16]) = 0;
    // Fills the device fields of INFO (3.2): otaState, verifySecondsLeft,
    // runningSlot, statusFlags bits 3 and 5, lastInvalidVersion, label,
    // runningImageId, lastInvalidImageId, rollback cause/detail, webUiVersion,
    // webUiId, webUiSize, uptimeS. The receiver sets everything else.
    virtual void fillInfo(InfoMsg &m) = 0;
    // 9.1: returns 0 and fills `out` when the work region is free, otherwise
    // the milliseconds until it may be (the 120 s grace after a web UI swap).
    virtual uint32_t acquireWorkArea(WorkArea &out) = 0;
    // The receiver no longer touches window/dict (session ended).
    virtual void releaseWorkArea() = 0;
    // Every state change, for UpdateGate (offerBusy, canRestart, ...).
    virtual void sessionStateChanged(uint8_t state) = 0;
};

// Fields of the confirmation dialog (7.2).
struct ConfirmInfo {
    uint32_t token;              // pass back with userAccept/userDecline
    uint8_t  senderAddress;
    uint8_t  senderName[16];     // raw UTF-8; sanitizeText() before LVGL
    uint8_t  senderKind;
    uint8_t  target, boardId, encoding;
    uint8_t  runningVersion[16]; // this firmware
    uint8_t  version[16];        // offered
    int8_t   versionRelation;    // compareVersions(offered, running): -1 older, 0 same, 1 newer, 2 n/a
    uint32_t imageSize, streamSize;
    uint32_t companionSize;      // 0 = no companion web UI
    uint16_t estSeconds;         // 0xFFFF = unknown
    uint16_t dialogSeconds;      // 60
};

// Snapshot of the session for the progress banner (7.3) and /api/info.
struct Snapshot {
    uint32_t token;
    uint8_t  state, reason, target, encoding, senderAddress, sessionId;
    uint8_t  version[16];
    uint32_t imageWritten, imageSize, streamAccepted, streamSize;
    uint32_t transferStartMs;    // first credit; 0 before
    uint16_t secondsLeft;        // as in STATUS; 0xFFFF = NA
};

// UI side (7.x). Calls are posted to the display task by the glue.
class IUpdUi {
public:
    virtual ~IUpdUi() {}
    // Open the modal dialog. Install acts after a 1.5 s hold that starts after
    // the 1.5 s guard; the receiver ignores an accept earlier than 3 s anyway.
    virtual void confirmOpen(const ConfirmInfo &info) = 0;
    // Close it: timeout, sender silent, sender ABORT, alarm, setting OFF,
    // pre-emption. Not called after the UI itself reported accept/decline.
    virtual void confirmClose(uint32_t token) = 0;
    // Any state change (banner on/off/text). Progress counters: snapshot().
    virtual void stateChanged(const Snapshot &s) = 0;
    // 7.6: "Update aborted:" + the reason text.
    virtual void failed(uint32_t token, uint8_t reason) = 0;
    // Firmware: mode 0/2 start the 10 s countdown, mode 1 shows the note.
    // Web UI: the toast "Web interface updated."
    virtual void applied(uint32_t token, uint8_t target, uint8_t mode, uint8_t detail) = 0;
    // 3.11 / 7.7: show the identify screen for `seconds`.
    virtual void identify(uint8_t seconds) = 0;
};

// ---- configuration -----------------------------------------------------------------------
struct Identity {
    uint8_t  boardId;            // NP_BOARD_ID
    uint16_t panelW, panelH;     // LCD_WIDTH / LCD_HEIGHT
    uint8_t  uniqueId[6];        // base MAC, display order
    uint8_t  fwVersion[16];      // FW_VERSION as str[16] (putVersionField)
};

struct Config {
    uint32_t maxFirmwareSize;    // inactive slot size - 4096 (INFO byte 21)
    uint32_t maxWebUiSize;       // one update region (INFO byte 25)
    uint16_t unitEstimateMs;     // hold-ms estimate per 4 KB unit (default 100)
    uint16_t flashGapMs;         // UPD_FLASH_GAP_MS between units (default 0)
    bool     resume;             // capability bit 3
    bool     identify;           // capability bit 4
    bool     companion;          // capability bit 5 (needs both targets)
};
Config defaultConfig();

struct Env {
    IUpdClock  *clock;
    IUpdLink   *link;
    IUpdHost   *host;
    IUpdUi     *ui;
    ISha256    *sha;
    IInflater  *inflater;        // null = no deflate (capability bit 2 off)
    IUpdTarget *firmware;        // null = firmware target unsupported
    IUpdTarget *webUi;           // null = web UI target unsupported
    Crc32Fn     crc32;           // null = crc32Update(); device: esp_rom_crc32_le wrapper
    uint8_t    *bounce;          // BOUNCE_BYTES, internal RAM, 4-byte aligned
};

// Counters for the [HB] log line and the tests.
struct Stats {
    uint32_t txDropped;          // IUpdLink::send() returned false
    uint32_t sessions;           // OFFERs that opened a dialog or were auto-accepted
    uint32_t chunksStored;
    uint32_t chunksDuplicate;
    uint32_t windows;
    uint32_t flashUnits;
    uint32_t ignored;            // well-formed but ignored (binding, direction, ...)
};

// ---- the receiver ------------------------------------------------------------------------
class Receiver {
public:
    Receiver();

    // Wires the receiver up. Returns false (and stays inert: no answers, no
    // reception proof) if a mandatory pointer is missing.
    bool begin(const Env &env, const Identity &id, const Config &cfg);

    // A reassembled PGN 126720 message: source and destination address from
    // the CAN id, `payload` from byte 0 (FE 9F ...). Non-protocol payloads
    // (other manufacturers, other types) are ignored here as well.
    void onMessage(uint8_t src, uint8_t dst, const uint8_t *payload, size_t len);

    // Timers and at most one unit of flash work. Call every N2K loop pass.
    void tick();

    // From the UI mailbox (7.1). `token` is ConfirmInfo::token / Snapshot::token.
    void userAccept(uint32_t token);    // Install held 1.5 s
    void userDecline(uint32_t token);   // Decline tapped
    void userCancel(uint32_t token);    // Cancel held 1.5 s on the progress banner
    void userIdentifyClosed();          // the identify screen was tapped away (7.7)

    uint8_t  state() const { return _s.state; }
    Snapshot snapshot() const;
    uint8_t  capabilities() const;
    // H5 (10.2): a well-formed sender->display message of this protocol,
    // addressed to us (or to 255 with a matching or wildcard filter), has been
    // received since begin().
    bool receptionProof() const { return _rxProof; }
    // True from CONFIRMING to APPLYING (the window in which UpdateGate blocks
    // WiFi OTA and restarts).
    bool sessionActive() const;
    const Stats &stats() const { return _stats; }

private:
    // One session; zeroed between sessions. Lives inside the Receiver, so its
    // place in memory is wherever the glue puts the Receiver object.
    struct Session {
        uint8_t  state;
        uint8_t  src, sid;          // the binding (5.1 rule 4)
        uint8_t  reason;            // failure reason (STATUS byte 6)
        uint32_t offerDigest;       // of the latest bound OFFER (5.3.3 duplicates)
        OfferMsg offer;             // the OFFER that opened the session
        uint32_t token;
        uint32_t tState, tHeard, tDialog;
        // transfer (5.4)
        uint32_t base, end;
        uint64_t bitmap;
        uint32_t crc;               // CRC-32 over stream bytes [0, base)
        uint32_t tGapRef, tA6, tStatusReply, transferStart;
        uint8_t  a4Count, belowBase;
        uint16_t malformed;
        bool     a6Any, statusReplyAny;
        bool     workHeld, targetOpen, viaCompanion;
        uint32_t workWaitMs;
        WorkArea work;
        // processing of the completed window (9.1-9.3)
        uint32_t procLen, procPos;  // window bytes, bytes consumed
        uint32_t imageWritten;
        uint64_t decoded;
        uint32_t dictOut, dictRd, pending;
        int      inflStatus;
        bool     inflDone, inflEnded, descDue, descChecked;
        // verification (5.5)
        uint8_t  vStep;
        uint32_t readPos;
        uint8_t  imageId[8];        // read back from flash at 0xB0
        // the stored RESULT (3.7)
        bool     haveResult;
        uint8_t  resCode, resPhase, resDetail;
        uint8_t  resSha[32];
        uint32_t tResult;
        // apply (5.6)
        uint8_t  applyMode;
        bool     applyBusy;
        uint32_t tApplyBusy, tApplyTry;
        // keepalive and flash pacing
        uint32_t tKeepalive, tUnitEnd, tTxWait;
        bool     txWaiting, unitDone;
    };

    // The user's answer to the last dialog (declined, or not confirmed within
    // 60 s): a retransmitted OFFER, and for a short while any OFFER of that
    // sender, gets the same answer instead of a new dialog.
    struct RejectMemo {
        bool     valid;
        uint8_t  src, sid, reason;
        uint32_t offerDigest;
        uint32_t t;
    };

    // incoming
    void handleQuery(uint8_t src, uint8_t dst, const uint8_t *p, size_t len);
    void handleOffer(uint8_t src, const uint8_t *p, size_t len);
    void handleData(uint8_t src, const uint8_t *p, size_t len);
    void handleApply(uint8_t src, const uint8_t *p, size_t len);
    void handleAbort(uint8_t src, const uint8_t *p, size_t len);
    void handleIdentify(const uint8_t *p, size_t len);

    // offer helpers
    uint8_t checkOfferTail(const OfferMsg &o);       // 5.3.1 steps 6-12
    static uint32_t offerDigest(const uint8_t *p);
    bool resumeMatches(const OfferMsg &o) const;
    void replyCurrent();                             // 5.3.4 table
    void startSession(uint8_t src, const uint8_t *raw, const OfferMsg &o, bool viaCompanion);

    // outgoing
    void send(uint8_t dst, uint8_t prio, const uint8_t *p, size_t n);
    void sendReply(uint8_t dst, uint8_t sid, uint8_t decision, uint8_t reason,
                   uint16_t seconds, uint8_t window);
    void sendAbort(uint8_t dst, uint8_t sid, uint8_t reason);
    void sendSessionUnknown(uint8_t dst, uint8_t sid);
    void sendInfo(uint8_t dst);
    void sendStatus(uint8_t dst, uint8_t sid);
    void sendAck();
    void sendResult();

    // state
    void enter(uint8_t st);
    void closeDialog(bool notifyUi, uint8_t rejectReason);
    void fail(uint8_t reason, uint8_t how);         // FAIL_* below
    void storeResult(uint8_t code, uint8_t phase, uint8_t detail, const uint8_t *sha);
    void cleanupTarget();
    void releaseWork();
    void toIdle();
    bool bound(uint8_t src, uint8_t sid) const;
    uint32_t companionSize() const;
    uint16_t secondsLeft() const;
    uint16_t holdEstimateMs() const;
    uint32_t now() { return _env.clock->millis(); }
    IUpdTarget *target() const;
    uint32_t crc(uint32_t prev, const uint8_t *p, size_t n) const;

    // work
    bool flashReady(uint32_t t);
    void flashDone(uint32_t t);
    void doPreparing(uint32_t t);
    void doProcessing(uint32_t t);
    bool writeUnitRaw();
    bool writeUnitDeflate();
    bool writeBounced(const uint8_t *src, uint32_t n);
    bool windowFinished() const;
    void finishWindow(uint32_t t);
    bool descriptorCheck();
    void doVerifying(uint32_t t);
    void doApplying(uint32_t t);
    void completeWindow(uint32_t t);

    Env        _env;
    Identity   _id;
    Config     _cfg;
    bool       _ready;
    bool       _rxProof;
    Session    _s;
    RejectMemo _memo;
    uint32_t   _tokenCounter;
    uint32_t   _tUnknown;       // last ABORT(SESSION_UNKNOWN)
    bool       _unknownSent;
    uint32_t   _tIdentifyFree;  // IDENTIFY is accepted again from here on
    bool       _identifySeen;
    Stats      _stats;
};

// Boot self-tests 2-4 of 8.1 through the same interfaces the receiver uses:
// 2 CRC-32 check value, 3 SHA-256("abc"), 4 the Appendix A inflate vector into
// `dict` (DICT_BYTES) with `inflaterState`. Inputs are copied into `bounce`
// first. Returns 0 when all pass, else the number of the first failing test.
// Self-test 1 (descriptor address) needs the linker and stays in the glue.
// `inflater` may be null: test 4 is then skipped (no deflate capability).
int runSelfTests(Crc32Fn crc, ISha256 &sha, IInflater *inflater, void *inflaterState,
                 uint8_t *dict, uint8_t *bounce);

}  // namespace n2kupd
