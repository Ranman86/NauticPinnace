// ============================================================================
// N2kUpdateCore.cpp - the receiver state machine; see N2kUpdateCore.h for the
// contract and docs/N2K-UPDATE.md sections 3, 5, 8 and 9 for the rules. Each
// rule below names the section it implements.
// ============================================================================
#include "N2kUpdateCore.h"
#include <string.h>

namespace n2kupd {

namespace {

// How a session ends in fail().
enum : uint8_t {
    FAIL_ABORT  = 0,   // display-side error during a transfer: ABORT(reason)
    FAIL_RESULT = 1,   // verification or apply error: the RESULT was already sent
    FAIL_SILENT = 2,   // the sender aborted: nothing is sent back (3.9)
};

// Verification steps (5.5).
enum : uint8_t {
    V_CHECKS = 0,      // steps 1-3: stream CRC, decoder/size, write-time SHA
    V_FINISH,          // step 4: esp_ota_end / close
    V_READBACK,        // step 5: read back and hash, 4 KB per tick
    V_IDENTITY,        // step 6: descriptor or gzip magic from flash
};

// PREPARING waits for the work region at most for the grace (9.1). A host that
// keeps it longer than that plus this margin is broken; the session then ends
// rather than blocking every other sender with BUSY for ever.
static constexpr uint32_t T_PREPARING_MAX_MS = T_REGION_GRACE_MS + 30000;

// A dialog the user declined, or one that timed out, keeps its sender from
// opening the next one at once: for this long every OFFER from that address
// gets the same answer. A sender stuck in a retry loop could otherwise cover
// the instruments with one dialog after the other, and the user would never
// get to the settings switch that turns updates off.
static constexpr uint32_t T_DIALOG_COOLDOWN_MS = 10000;

// IDENTIFY (3.11): the 10 s limit counts from the END of the previous identify
// screen, so a sender repeating it cannot keep the screen up for good; after
// the user tapped the screen away, IDENTIFY is ignored this long.
static constexpr uint32_t T_IDENTIFY_TAP_QUIET_MS = 60000;

// ACK hold ms for a deflate window (3.6): the window's share of the image at
// the average ratio, times this. Highly compressible windows decode to several
// times the average (measured up to 2.3x); the hold is "the longest expected
// time", and a long one costs nothing, because STATUS every second keeps the
// sender's timer running and A4 repeats a lost A2.
static constexpr uint32_t HOLD_DEFLATE_FACTOR = 4;

static const uint8_t GZIP_MAGIC[3] = { 0x1F, 0x8B, 0x08 };

inline uint32_t since(uint32_t now, uint32_t t) { return now - t; }
inline uint32_t minU32(uint32_t a, uint32_t b) { return a < b ? a : b; }
inline uint16_t ceilSeconds(uint32_t ms) {
    const uint32_t s = (ms + 999) / 1000;
    return (uint16_t)(s > 0xFFFE ? 0xFFFE : s);
}

}  // namespace

Config defaultConfig() {
    Config c;
    c.maxFirmwareSize = 0;
    c.maxWebUiSize    = 0x20000;
    c.unitEstimateMs  = 100;
    c.flashGapMs      = 0;
    c.resume          = true;
    c.identify        = true;
    c.companion       = true;
    return c;
}

Receiver::Receiver() {
    memset(&_env, 0, sizeof(_env));
    memset(&_id, 0, sizeof(_id));
    _cfg = defaultConfig();
    _ready = false;
    _rxProof = false;
    memset(&_s, 0, sizeof(_s));
    memset(&_memo, 0, sizeof(_memo));
    _tokenCounter = 0;
    _tUnknown = 0;
    _unknownSent = false;
    _tIdentifyFree = 0;
    _identifySeen = false;
    memset(&_stats, 0, sizeof(_stats));
}

bool Receiver::begin(const Env &env, const Identity &id, const Config &cfg) {
    _ready = false;
    if (!env.clock || !env.link || !env.host || !env.ui || !env.sha || !env.bounce) return false;
    _env = env;
    _id = id;
    _cfg = cfg;
    memset(&_s, 0, sizeof(_s));
    memset(&_memo, 0, sizeof(_memo));
    _s.state = STATE_IDLE;
    _ready = true;
    return true;
}

// ---- small helpers -------------------------------------------------------------------
IUpdTarget *Receiver::target() const {
    return _s.offer.target == TARGET_FIRMWARE ? _env.firmware : _env.webUi;
}

uint32_t Receiver::crc(uint32_t prev, const uint8_t *p, size_t n) const {
    return _env.crc32 ? _env.crc32(prev, p, n) : crc32Update(prev, p, n);
}

bool Receiver::bound(uint8_t src, uint8_t sid) const {
    return _s.state != STATE_IDLE && src == _s.src && sid == _s.sid;
}

// The companion web UI named by the OFFER (5.3.6), or 0 when this display has
// no companion capability: then no approval is stored at APPLY, so the dialog
// must not promise one either. The OFFER itself stays untouched (RESUME
// compares its fields).
uint32_t Receiver::companionSize() const {
    if (_s.offer.target != TARGET_FIRMWARE || !(capabilities() & CAP_COMPANION)) return 0;
    return _s.offer.companionSize;
}

bool Receiver::sessionActive() const {
    return _s.state >= STATE_CONFIRMING && _s.state <= STATE_APPLYING;
}

uint8_t Receiver::capabilities() const {
    if (!_ready) return 0;
    // 4.4: 0 if the self-tests failed or the regions could not be reserved.
    if (!_env.host->selfTestsPassed() || !_env.host->regionsReserved()) return 0;
    uint8_t c = 0;
    if (_env.firmware) c |= CAP_FIRMWARE;
    if (_env.webUi) c |= CAP_WEBUI;
    if (_env.firmware && _env.inflater) c |= CAP_DEFLATE;
    if (_cfg.resume) c |= CAP_RESUME;
    if (_cfg.identify) c |= CAP_IDENTIFY;
    if (_cfg.companion && _env.firmware && _env.webUi) c |= CAP_COMPANION;
    return c;
}

Snapshot Receiver::snapshot() const {
    Snapshot s;
    memset(&s, 0, sizeof(s));
    s.token = _s.token;
    s.state = _s.state;
    s.reason = _s.reason;
    s.target = _s.offer.target;
    s.encoding = _s.offer.encoding;
    s.senderAddress = _s.src;
    s.sessionId = _s.sid;
    memcpy(s.version, _s.offer.fwVersion, 16);
    s.imageWritten = _s.imageWritten;
    s.imageSize = _s.offer.imageSize;
    s.streamAccepted = _s.base;
    s.streamSize = _s.offer.streamSize;
    s.transferStartMs = _s.transferStart;
    s.secondsLeft = secondsLeft();
    return s;
}

// STATUS byte 23 (3.10): seconds left in the current phase.
uint16_t Receiver::secondsLeft() const {
    const uint32_t t = _env.clock ? _env.clock->millis() : 0;
    switch (_s.state) {
        case STATE_CONFIRMING: {
            const uint32_t e = since(t, _s.tDialog);
            return ceilSeconds(e >= T_DIALOG_MS ? 0 : T_DIALOG_MS - e);
        }
        case STATE_PREPARING:
            return ceilSeconds(_s.workWaitMs);
        case STATE_VERIFYING: {
            // A rough estimate of the remaining read-back; the sender only uses
            // it for display, its 120 s timer is extended by every STATUS.
            const uint32_t left = _s.offer.imageSize > _s.readPos ? _s.offer.imageSize - _s.readPos : 0;
            const uint32_t units = left / FLASH_UNIT + 1;
            return ceilSeconds(units * (uint32_t)(_cfg.unitEstimateMs / 4 + 1) + 1000);
        }
        case STATE_READY_TO_APPLY: {
            const uint32_t e = since(t, _s.tState);
            return ceilSeconds(e >= T_APPLY_WAIT_MS ? 0 : T_APPLY_WAIT_MS - e);
        }
        default:
            return 0xFFFF;
    }
}

// ACK byte 26 (3.6): with no credit, the longest expected time until the next
// ACK. Units of the rest of the window, one unit for the descriptor check, and
// the TX-idle wait in front of a unit. A deflate window's output is unknown
// until it is decoded: HOLD_DEFLATE_FACTOR times its average share, but never
// more than the image still has to come.
uint16_t Receiver::holdEstimateMs() const {
    uint64_t rem;
    if (_s.offer.encoding == ENC_DEFLATE && _s.offer.streamSize) {
        const uint64_t avg =
            (uint64_t)(_s.procLen - _s.procPos) * _s.offer.imageSize / _s.offer.streamSize;
        const uint64_t left = _s.offer.imageSize > _s.imageWritten
                                  ? (uint64_t)(_s.offer.imageSize - _s.imageWritten) : 0;
        rem = (uint64_t)_s.pending + avg * HOLD_DEFLATE_FACTOR;
        if (rem > left) rem = left;
    } else {
        rem = _s.procLen - _s.procPos;
    }
    const uint64_t units = rem / FLASH_UNIT + 2;
    const uint64_t ms = units * (_cfg.unitEstimateMs ? _cfg.unitEstimateMs : 100) + T_TX_IDLE_MAX_MS;
    return (uint16_t)(ms > 0xFFFE ? 0xFFFE : ms);
}

// ---- outgoing ---------------------------------------------------------------------------
void Receiver::send(uint8_t dst, uint8_t prio, const uint8_t *p, size_t n) {
    if (!n) return;
    if (!_env.link->send(dst, prio, p, n)) _stats.txDropped++;
}

void Receiver::sendReply(uint8_t dst, uint8_t sid, uint8_t decision, uint8_t reason,
                         uint16_t seconds, uint8_t window) {
    OfferReplyMsg m;
    m.version = PROTO_VERSION; m.session = sid; m.decision = decision; m.reason = reason;
    m.seconds = seconds; m.windowChunks = window;
    uint8_t b[LEN_OFFER_REPLY];
    send(dst, PRIO_CONTROL, b, buildOfferReply(m, b, sizeof(b)));
}

void Receiver::sendAbort(uint8_t dst, uint8_t sid, uint8_t reason) {
    AbortMsg m;
    m.version = PROTO_VERSION; m.session = sid; m.reason = reason;
    uint8_t b[LEN_ABORT];
    send(dst, PRIO_CONTROL, b, buildAbort(m, b, sizeof(b)));
}

// 5.1 rule 4: a single ABORT(SESSION_UNKNOWN) per second for stale DATA,
// APPLY or QUERY - one limiter for all senders, so a flood cannot turn the
// display into a transmitter.
void Receiver::sendSessionUnknown(uint8_t dst, uint8_t sid) {
    const uint32_t t = now();
    if (_unknownSent && since(t, _tUnknown) < T_SESSION_UNKNOWN_MS) return;
    _unknownSent = true;
    _tUnknown = t;
    sendAbort(dst, sid, RSN_SESSION_UNKNOWN);
}

void Receiver::sendInfo(uint8_t dst) {
    InfoMsg m;
    memset(&m, 0, sizeof(m));
    _env.host->fillInfo(m);
    IUpdHost &h = *_env.host;
    m.version = PROTO_VERSION;
    m.session = _s.state != STATE_IDLE ? _s.sid : 0;
    m.protoMin = PROTO_MIN;
    m.protoMax = PROTO_MAX;
    m.boardId = _id.boardId;
    m.capabilities = capabilities();
    memcpy(m.uniqueId, _id.uniqueId, 6);
    m.sessionState = _s.state;
    uint8_t f = m.statusFlags & (SF_ROLLED_BACK | SF_COMPANION_WAITING);
    if (h.acceptUpdates()) f |= SF_ACCEPT_UPDATES;
    if (h.alarmActive()) f |= SF_ALARM;
    if (h.restartPending()) f |= SF_RESTART_PENDING;
    if (h.setupDone() && h.selfTestsPassed() && h.regionsReserved()) f |= SF_READY;
    if (h.wifiOtaActive()) f |= SF_WIFI_OTA;
    m.statusFlags = f;
    m.maxFirmwareSize = _cfg.maxFirmwareSize;
    m.maxWebUiSize = _cfg.maxWebUiSize;
    m.maxChunkSize = (uint8_t)CHUNK_SIZE;
    m.maxWindowChunks = (uint8_t)WINDOW_CHUNKS;
    memcpy(m.runningVersion, _id.fwVersion, 16);
    uint8_t b[LEN_INFO];
    send(dst, PRIO_CONTROL, b, buildInfo(m, b, sizeof(b)));
}

void Receiver::sendStatus(uint8_t dst, uint8_t sid) {
    StatusMsg m;
    m.version = PROTO_VERSION;
    m.session = sid;
    m.state = _s.state;
    m.reason = _s.reason;
    m.streamAccepted = _s.base;
    m.streamSize = _s.offer.streamSize;
    m.imageWritten = _s.imageWritten;
    m.imageSize = _s.offer.imageSize;
    m.seconds = secondsLeft();
    uint8_t b[LEN_STATUS];
    send(dst, PRIO_CONTROL, b, buildStatus(m, b, sizeof(b)));
}

// The current ACK (3.6): RECEIVING carries the credit and the bitmap,
// PROCESSING carries no credit (end == base) and the hold estimate.
void Receiver::sendAck() {
    AckMsg m;
    m.version = PROTO_VERSION;
    m.session = _s.sid;
    m.base = _s.base;
    m.crc = _s.crc;
    if (_s.state == STATE_PROCESSING) {
        m.state = STATE_PROCESSING;
        m.end = _s.base;
        m.bitmap = 0;
        m.holdMs = holdEstimateMs();
    } else {
        m.state = STATE_RECEIVING;
        m.end = _s.end;
        m.bitmap = _s.bitmap;
        m.holdMs = 0;
    }
    uint8_t b[LEN_ACK];
    send(_s.src, PRIO_CONTROL, b, buildAck(m, b, sizeof(b)));
    _s.belowBase = 0;                  // A6 counts "since the last ACK"
}

void Receiver::sendResult() {
    ResultMsg m;
    m.version = PROTO_VERSION;
    m.session = _s.sid;
    m.code = _s.resCode;
    m.phase = _s.resPhase;
    m.detail = _s.resDetail;
    memcpy(m.sha256, _s.resSha, 32);
    uint8_t b[LEN_RESULT];
    send(_s.src, PRIO_CONTROL, b, buildResult(m, b, sizeof(b)));
    _s.tResult = now();
}

void Receiver::storeResult(uint8_t code, uint8_t phase, uint8_t detail, const uint8_t *sha) {
    _s.haveResult = true;
    _s.resCode = code;
    _s.resPhase = phase;
    _s.resDetail = detail;
    if (sha) {
        if (sha != _s.resSha) memcpy(_s.resSha, sha, 32);
    } else {
        memset(_s.resSha, 0, 32);      // "all zero if not computed" (3.7)
    }
}

// ---- state changes -----------------------------------------------------------------------
void Receiver::enter(uint8_t st) {
    const uint32_t t = now();
    _s.state = st;
    _s.tState = t;
    _s.tKeepalive = t;
    _env.host->sessionStateChanged(st);
    _env.ui->stateChanged(snapshot());
}

void Receiver::cleanupTarget() {
    if (_s.targetOpen) {
        _s.targetOpen = false;
        IUpdTarget *tg = target();
        if (tg) tg->abort();
    }
}

void Receiver::releaseWork() {
    if (_s.workHeld) {
        _s.workHeld = false;
        memset(&_s.work, 0, sizeof(_s.work));
        _env.host->releaseWorkArea();
    }
}

void Receiver::toIdle() {
    cleanupTarget();
    releaseWork();
    memset(&_s, 0, sizeof(_s));
    enter(STATE_IDLE);
}

// CONFIRMING ends (5.3.2). `rejectReason` != 0 sends REJECTED(reason);
// `notifyUi` closes the dialog for causes the UI did not report itself.
// Only the user's own answer is remembered - a decline, or no answer within
// 60 s: a retransmitted OFFER gets it again instead of a second dialog, and
// for T_DIALOG_COOLDOWN_MS so does every other OFFER from that sender. Alarm
// and setting are not remembered; they are evaluated afresh (5.3.1 steps 4
// and 12), so an OFFER after the alarm cleared opens its dialog.
void Receiver::closeDialog(bool notifyUi, uint8_t rejectReason) {
    if (rejectReason) {
        sendReply(_s.src, _s.sid, DECISION_REJECTED, rejectReason, 0, 0);
        if (rejectReason == RSN_DECLINED_BY_USER || rejectReason == RSN_CONFIRM_TIMEOUT) {
            _memo.valid = true;
            _memo.src = _s.src;
            _memo.sid = _s.sid;
            _memo.reason = rejectReason;
            _memo.offerDigest = _s.offerDigest;
            _memo.t = now();
        }
    }
    if (notifyUi) _env.ui->confirmClose(_s.token);
    toIdle();
}

void Receiver::fail(uint8_t reason, uint8_t how) {
    if (reason == RSN_OK) reason = RSN_INTERNAL_ERROR;
    if (how == FAIL_ABORT) sendAbort(_s.src, _s.sid, reason);
    _s.reason = reason;
    cleanupTarget();
    releaseWork();
    const uint32_t token = _s.token;
    enter(STATE_FAILED);
    _env.ui->failed(token, reason);
}

// ---- incoming -----------------------------------------------------------------------------
void Receiver::onMessage(uint8_t src, uint8_t dst, const uint8_t *p, size_t len) {
    if (!_ready) return;
    const uint8_t type = msgType(p, len);
    if (!type) return;                                     // not this protocol (5.1 rule 1)
    if (!isSenderToDisplay(type)) { _stats.ignored++; return; }   // wrong direction (2.5)

    // 5.1 rule 2: addressed to us; only QUERY may come to 255.
    const uint8_t own = _env.link->ownAddress();
    if (dst != own && !(type == MSG_QUERY && dst == ADDR_BROADCAST)) { _stats.ignored++; return; }

    if (len < minLength(type)) {
        // 5.3.1 step 1 answers an OFFER it cannot read; everything else that is
        // shorter than its v1 length is ignored (2.5).
        if (type == MSG_OFFER && len >= LEN_CTRL_HDR)
            sendReply(src, p[4], DECISION_REJECTED, RSN_UNSUPPORTED_PROTOCOL, 0, 0);
        else
            _stats.ignored++;
        return;
    }

    // QUERY carries a unique id filter: FF x 6 = any display, else only that
    // one answers (3.1). A QUERY for another display is not "addressed to this
    // display" for H5 either.
    if (type == MSG_QUERY) {
        const uint8_t *f = p + 6;
        if (!isAllFF(f, 6) && memcmp(f, _id.uniqueId, 6) != 0) { _stats.ignored++; return; }
    }

    // 5.1 rule 3: control messages must carry a supported version.
    if (type != MSG_DATA) {
        const uint8_t ver = p[3];
        if (ver < PROTO_MIN || ver > PROTO_MAX) {
            // QUERY(INFO) is answered anyway, so a newer sender can read protoMax.
            if (type == MSG_QUERY && p[5] == QUERY_INFO) sendInfo(src);
            else if (type == MSG_OFFER)
                sendReply(src, p[4], DECISION_REJECTED, RSN_UNSUPPORTED_PROTOCOL, 0, 0);
            else _stats.ignored++;
            return;
        }
    }

    // H5 (10.2): a well-formed sender->display message of this protocol,
    // addressed to us or to 255 with a matching or wildcard filter.
    _rxProof = true;

    switch (type) {
        case MSG_QUERY:    handleQuery(src, dst, p, len); break;
        case MSG_OFFER:    handleOffer(src, p, len);      break;
        case MSG_DATA:     handleData(src, p, len);       break;
        case MSG_APPLY:    handleApply(src, p, len);      break;
        case MSG_ABORT:    handleAbort(src, p, len);      break;
        case MSG_IDENTIFY: handleIdentify(p, len);        break;
        default: break;
    }
}

// 3.1: exactly one answer, unicast to the requester.
void Receiver::handleQuery(uint8_t src, uint8_t dst, const uint8_t *p, size_t len) {
    (void)dst;
    QueryMsg q;
    if (!parseQuery(p, len, q)) return;
    const bool b = q.session != 0 && bound(src, q.session);
    if (b) _s.tHeard = now();
    switch (q.what) {
        case QUERY_INFO:
            sendInfo(src);
            return;
        case QUERY_STATUS:
        case QUERY_ACK:
        case QUERY_RESULT:
            if (q.session != 0 && !b) { sendSessionUnknown(src, q.session); return; }
            if (!b) {
                // No session named: the STATUS of whatever is going on.
                sendStatus(src, _s.state != STATE_IDLE ? _s.sid : 0);
                return;
            }
            if (q.what == QUERY_ACK &&
                (_s.state == STATE_RECEIVING || _s.state == STATE_PROCESSING)) {
                sendAck();                                  // A5
                return;
            }
            if (q.what == QUERY_RESULT && _s.haveResult) { sendResult(); return; }
            sendStatus(_s.src, _s.sid);
            return;
        default:
            _stats.ignored++;
            return;
    }
}

// 5.3.3 "byte-identical apart from the RESUME bit", kept as a CRC-32 of the
// v1 bytes with that bit masked instead of a 131-byte copy: internal RAM is
// the scarcest resource on the device. A collision could only make the
// display repeat its current reply to the session's own sender and session id;
// it never changes the state or the binding.
uint32_t Receiver::offerDigest(const uint8_t *p) {
    const uint8_t flags = (uint8_t)(p[8] & (uint8_t)~OFFER_F_RESUME);
    uint32_t c = crc32Update(0, p, 8);
    c = crc32Update(c, &flags, 1);
    return crc32Update(c, p + 9, LEN_OFFER - 9);
}

// 5.3.4: the fields that identify exactly this image and this stream.
bool Receiver::resumeMatches(const OfferMsg &o) const {
    const OfferMsg &h = _s.offer;
    return o.target == h.target && o.encoding == h.encoding && o.chunkSize == h.chunkSize &&
           o.imageSize == h.imageSize && o.streamSize == h.streamSize &&
           memcmp(o.sha256, h.sha256, 32) == 0 && o.streamCrc == h.streamCrc &&
           memcmp(o.companionSha, h.companionSha, 32) == 0 &&
           o.companionSize == h.companionSize;
}

// 5.3.3 / 5.3.4 reply table.
void Receiver::replyCurrent() {
    switch (_s.state) {
        case STATE_CONFIRMING:
            sendReply(_s.src, _s.sid, DECISION_PENDING_USER, 0, secondsLeft(), 0);
            return;
        case STATE_PREPARING:
        case STATE_VERIFYING:
            sendReply(_s.src, _s.sid, DECISION_ACCEPTED, 0, 0, (uint8_t)WINDOW_CHUNKS);
            sendStatus(_s.src, _s.sid);
            return;
        case STATE_RECEIVING:
        case STATE_PROCESSING:
            sendReply(_s.src, _s.sid, DECISION_ACCEPTED, 0, 0, (uint8_t)WINDOW_CHUNKS);
            sendAck();
            return;
        case STATE_READY_TO_APPLY:
        case STATE_APPLYING:
        case STATE_DONE:
            sendReply(_s.src, _s.sid, DECISION_ACCEPTED, 0, 0, (uint8_t)WINDOW_CHUNKS);
            if (_s.haveResult) sendResult(); else sendStatus(_s.src, _s.sid);
            return;
        default:
            return;
    }
}

// 5.3.1 steps 6 to 12, in that order.
uint8_t Receiver::checkOfferTail(const OfferMsg &o) {
    IUpdHost &h = *_env.host;
    // 6
    if (h.runningPendingVerify()) return RSN_PENDING_VERIFY;
    if (h.restartPending()) return RSN_RESTART_PENDING;
    // 7
    const bool fw = o.target == TARGET_FIRMWARE;
    const bool web = o.target == TARGET_WEBUI;
    if (!(fw && _env.firmware) && !(web && _env.webUi)) return RSN_UNSUPPORTED_TARGET;
    if (o.encoding != ENC_RAW && !(o.encoding == ENC_DEFLATE && fw && _env.inflater))
        return RSN_UNSUPPORTED_ENCODING;
    // 8
    if (o.session == 0 || o.session == 255) return RSN_BAD_REQUEST;
    if (o.chunkSize != CHUNK_SIZE) return RSN_BAD_REQUEST;
    if (o.imageSize == 0 || o.streamSize == 0 || o.streamSize > MAX_STREAM_SIZE) return RSN_BAD_REQUEST;
    if (o.encoding == ENC_RAW && o.streamSize != o.imageSize) return RSN_BAD_REQUEST;
    if (o.encoding == ENC_DEFLATE &&
        (uint64_t)o.streamSize > (uint64_t)o.imageSize + o.imageSize / 1000 + 1024)
        return RSN_BAD_REQUEST;
    if (!isValidVersionField(o.fwVersion)) return RSN_BAD_REQUEST;
    const bool compShaZero = isAllZero(o.companionSha, 32);
    if (web && (!compShaZero || o.companionSize != 0)) return RSN_BAD_REQUEST;
    if ((o.companionSize == 0) != compShaZero) return RSN_BAD_REQUEST;
    if (o.companionSize > _cfg.maxWebUiSize) return RSN_BAD_REQUEST;
    // 9
    if (o.imageSize > (fw ? _cfg.maxFirmwareSize : _cfg.maxWebUiSize)) return RSN_TOO_LARGE;
    if (web || o.companionSize) {
        const uint32_t freeB = h.fsFreeBytes();
        const uint32_t room = freeB > WEBUI_FREE_MARGIN ? freeB - WEBUI_FREE_MARGIN : 0;
        if (web && o.imageSize > room) return RSN_NO_SPACE;
        if (o.companionSize > room) return RSN_NO_SPACE;
    }
    // 10
    if (fw && o.boardId != _id.boardId) return RSN_WRONG_BOARD;
    if (web && o.boardId != BOARD_ANY && o.boardId != _id.boardId) return RSN_WRONG_BOARD;
    // 11
    if (web && !(o.flags & OFFER_F_ALLOW_VERSION_MISMATCH) &&
        memcmp(o.fwVersion, _id.fwVersion, 16) != 0)
        return RSN_VERSION_MISMATCH;
    // 12
    if (h.alarmActive()) return RSN_ALARM_ACTIVE;
    return RSN_OK;
}

void Receiver::startSession(uint8_t src, const uint8_t *raw, const OfferMsg &o, bool viaCompanion) {
    const uint32_t t = now();
    memset(&_s, 0, sizeof(_s));
    _s.src = src;
    _s.sid = o.session;
    _s.offerDigest = offerDigest(raw);
    _s.offer = o;
    if (++_tokenCounter == 0) ++_tokenCounter;
    _s.token = _tokenCounter;
    _s.tHeard = t;
    _s.viaCompanion = viaCompanion;
    _stats.sessions++;

    if (viaCompanion) {
        // 5.3.6 step 3: approved together with the firmware; no dialog.
        sendReply(src, o.session, DECISION_ACCEPTED, 0, 0, (uint8_t)WINDOW_CHUNKS);
        enter(STATE_PREPARING);
        return;
    }

    sendReply(src, o.session, DECISION_PENDING_USER, 0, (uint16_t)(T_DIALOG_MS / 1000), 0);
    _s.tDialog = t;
    enter(STATE_CONFIRMING);

    ConfirmInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.token = _s.token;
    ci.senderAddress = src;
    memcpy(ci.senderName, o.senderName, 16);
    ci.senderKind = o.senderKind;
    ci.target = o.target;
    ci.boardId = o.boardId;
    ci.encoding = o.encoding;
    memcpy(ci.runningVersion, _id.fwVersion, 16);
    memcpy(ci.version, o.fwVersion, 16);
    char a[17], b[17];
    memcpy(a, o.fwVersion, 16); a[16] = 0;
    memcpy(b, _id.fwVersion, 16); b[16] = 0;
    ci.versionRelation = (int8_t)compareVersions(a, b);
    ci.imageSize = o.imageSize;
    ci.streamSize = o.streamSize;
    ci.companionSize = companionSize();
    ci.estSeconds = o.estSeconds;
    ci.dialogSeconds = (uint16_t)(T_DIALOG_MS / 1000);
    _env.ui->confirmOpen(ci);
}

void Receiver::handleOffer(uint8_t src, const uint8_t *p, size_t len) {
    OfferMsg o;
    if (!parseOffer(p, len, o)) return;
    const uint32_t t = now();
    IUpdHost &h = *_env.host;

    // 5.3.1 step 2
    if (memcmp(o.targetUid, _id.uniqueId, 6) != 0) {
        sendReply(src, o.session, DECISION_REJECTED, RSN_WRONG_DEVICE, 0, 0);
        return;
    }
    // step 3
    uint8_t r = RSN_OK;
    if (!h.setupDone()) r = RSN_NOT_READY;
    else if (!h.selfTestsPassed()) r = RSN_INTERNAL_ERROR;
    else if (!h.regionsReserved()) r = RSN_OUT_OF_MEMORY;
    // step 4
    else if (!h.acceptUpdates()) r = RSN_DISABLED;
    if (r) { sendReply(src, o.session, DECISION_REJECTED, r, 0, 0); return; }

    // step 5: duplicate (5.3.3), RESUME (5.3.4), pre-emption (5.3.5), BUSY.
    const uint8_t st = _s.state;
    const uint32_t digest = offerDigest(p);
    if (st >= STATE_CONFIRMING && st <= STATE_DONE) {
        if (src == _s.src && o.session == _s.sid && digest == _s.offerDigest) {
            _s.tHeard = t;
            replyCurrent();
            return;
        }
        if ((o.flags & OFFER_F_RESUME) && _cfg.resume && resumeMatches(o)) {
            // 5.3.4 against 5.8 ("a second OFFER gets BUSY"): a RESUME from
            // ANOTHER address takes the session over only once its sender has
            // been silent for more than 5 s, as for a pre-emption (5.3.5).
            // Otherwise a second sender that happens to offer the same image
            // would take a live transfer away from the first one.
            if (src != _s.src && since(t, _s.tHeard) <= T_PREEMPT_SILENCE_MS) {
                sendReply(src, o.session, DECISION_REJECTED, RSN_BUSY, 0, 0);
                return;
            }
            // Rebind without asking again: the user approved exactly this image.
            // The session keeps the fields of the OFFER that opened it.
            _s.src = src;
            _s.sid = o.session;
            _s.offerDigest = digest;
            _s.tHeard = t;
            replyCurrent();
            return;
        }
    }
    // The user's answer to the last dialog (closeDialog()): a retransmission
    // of that OFFER gets it again for 30 s, any other OFFER from the same
    // sender for T_DIALOG_COOLDOWN_MS - neither opens a dialog.
    if (_memo.valid && src == _memo.src) {
        const uint32_t e = since(t, _memo.t);
        const bool same = o.session == _memo.sid && digest == _memo.offerDigest;
        if (e < T_DIALOG_COOLDOWN_MS || (same && e < T_END_REPORT_MS)) {
            sendReply(src, o.session, DECISION_REJECTED, _memo.reason, 0, 0);
            return;
        }
    }
    if (sessionActive()) {
        if (src == _s.src && since(t, _s.tHeard) > T_PREEMPT_SILENCE_MS &&
            st != STATE_APPLYING) {
            // 5.3.5: the sender replaces its own stale session. No ABORT, no
            // failure message; the new OFFER is handled as new below.
            if (st == STATE_CONFIRMING) _env.ui->confirmClose(_s.token);
            toIdle();
        } else {
            sendReply(src, o.session, DECISION_REJECTED, RSN_BUSY, 0, 0);
            return;
        }
    }
    if (h.wifiOtaActive()) {
        sendReply(src, o.session, DECISION_REJECTED, RSN_BUSY, 0, 0);
        return;
    }

    // steps 6-12
    r = checkOfferTail(o);
    if (r) { sendReply(src, o.session, DECISION_REJECTED, r, 0, 0); return; }

    const bool viaCompanion = o.target == TARGET_WEBUI && (capabilities() & CAP_COMPANION) &&
                              h.companionApproved(o.sha256, o.imageSize, o.fwVersion);
    startSession(src, p, o, viaCompanion);
}

// 5.4: the display checks each DATA.
void Receiver::handleData(uint8_t src, const uint8_t *p, size_t len) {
    DataMsg d;
    if (!parseData(p, len, d)) { _stats.ignored++; return; }
    const uint32_t t = now();
    if (!bound(src, d.session)) { sendSessionUnknown(src, d.session); return; }
    _s.tHeard = t;

    if (_s.state != STATE_RECEIVING && _s.state != STATE_PROCESSING) {
        // The sender missed a state change (the last ACK, the dialog, a
        // failure): tell it where we are, at most every 250 ms.
        if (!_s.statusReplyAny || since(t, _s.tStatusReply) >= T_A6_INTERVAL_MS) {
            _s.statusReplyAny = true;
            _s.tStatusReply = t;
            sendStatus(_s.src, _s.sid);
        }
        return;
    }

    const uint32_t S = _s.offer.streamSize;
    const uint32_t expected = d.offset < S ? minU32(CHUNK_SIZE, S - d.offset) : CHUNK_SIZE;
    if (d.offset % CHUNK_SIZE != 0 || d.len != expected) {
        if (++_s.malformed >= MAX_MALFORMED_DATA) fail(RSN_BAD_REQUEST, FAIL_ABORT);
        return;
    }
    _s.tGapRef = t;                    // any DATA ends a gap (A4)
    _s.a4Count = 0;

    // A6: outside [base, end) - at once beyond the window, on the third
    // repeat below it (since the last ACK); at most one ACK per 250 ms.
    if (d.offset >= _s.end || d.offset < _s.base) {
        const bool want = d.offset >= _s.end || ++_s.belowBase >= A6_BELOW_BASE_COUNT;
        if (want && (!_s.a6Any || since(t, _s.tA6) >= T_A6_INTERVAL_MS)) {
            _s.a6Any = true;
            _s.tA6 = t;
            sendAck();
        }
        return;
    }

    // In the window (RECEIVING only: in PROCESSING end == base).
    const uint32_t rel = d.offset - _s.base;
    const uint32_t idx = rel / CHUNK_SIZE;
    const uint64_t bit = (uint64_t)1 << idx;
    if (_s.bitmap & bit) { _stats.chunksDuplicate++; return; }
    memcpy(_s.work.window + rel, d.data, d.len);
    _s.bitmap |= bit;
    _stats.chunksStored++;

    const uint32_t n = (_s.end - _s.base + CHUNK_SIZE - 1) / CHUNK_SIZE;
    const uint64_t full = n >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << n) - 1);
    if (_s.bitmap == full) { completeWindow(t); return; }
    if (idx == n - 1) sendAck();       // A3: the last chunk is in, holes remain
}

// A1: the window is complete. The CRC is taken over it before anything
// touches the buffer, then base moves to end and credit is withdrawn.
void Receiver::completeWindow(uint32_t t) {
    (void)t;
    const uint32_t len = _s.end - _s.base;
    _s.crc = crc(_s.crc, _s.work.window, len);
    _s.procLen = len;
    _s.procPos = 0;
    _s.inflEnded = false;
    _s.base = _s.end;
    _s.bitmap = 0;
    _stats.windows++;
    enter(STATE_PROCESSING);
    sendAck();
}

void Receiver::handleApply(uint8_t src, const uint8_t *p, size_t len) {
    ApplyMsg a;
    if (!parseApply(p, len, a)) return;
    if (!bound(src, a.session)) { sendSessionUnknown(src, a.session); return; }
    _s.tHeard = now();
    switch (_s.state) {
        case STATE_READY_TO_APPLY:
            if (a.mode > APPLY_NOW_NO_POLL) {
                // Not a v1 mode: activating with unknown semantics is not safe.
                sendStatus(_s.src, _s.sid);
                return;
            }
            // 3.8: the web UI treats 1 and 2 as 0.
            _s.applyMode = _s.offer.target == TARGET_WEBUI ? (uint8_t)APPLY_NOW : a.mode;
            _s.applyBusy = false;
            enter(STATE_APPLYING);
            return;
        case STATE_APPLYING:
            return;                     // idempotent: the RESULT follows (5.6)
        case STATE_DONE:
            sendResult();
            return;
        default:
            if (_s.haveResult) sendResult(); else sendStatus(_s.src, _s.sid);
            return;
    }
}

void Receiver::handleAbort(uint8_t src, const uint8_t *p, size_t len) {
    AbortMsg a;
    if (!parseAbort(p, len, a)) return;
    if (!bound(src, a.session)) { _stats.ignored++; return; }   // 3.9: not ours, ignored
    _s.tHeard = now();
    if (_s.state == STATE_CONFIRMING) {
        closeDialog(true, 0);   // 5.3.2: the dialog closes, nothing is sent
        return;
    }
    if (_s.state >= STATE_PREPARING && _s.state <= STATE_READY_TO_APPLY)
        fail(a.reason ? a.reason : (uint8_t)RSN_SENDER_CANCELLED, FAIL_SILENT);
    // APPLYING, DONE, FAILED: ignored (5.9).
}

void Receiver::handleIdentify(const uint8_t *p, size_t len) {
    IdentifyMsg m;
    if (!parseIdentify(p, len, m)) return;
    if (!_cfg.identify) return;
    if (memcmp(m.targetUid, _id.uniqueId, 6) != 0) return;
    if (m.seconds == 0) return;
    if (_s.state == STATE_CONFIRMING || _env.host->restartCountdownOpen() ||
        _env.host->alarmActive())
        return;
    const uint32_t t = now();
    // "At most once per 10 s" (3.11), counted from the end of the previous
    // identify screen: an open one is never extended or reopened.
    if (_identifySeen && (int32_t)(t - _tIdentifyFree) < 0) return;
    const uint8_t secs = m.seconds > IDENTIFY_MAX_S ? IDENTIFY_MAX_S : m.seconds;
    _identifySeen = true;
    _tIdentifyFree = t + (uint32_t)secs * 1000UL + T_IDENTIFY_INTERVAL_MS;
    _env.ui->identify(secs);
}

// ---- user input (7.1) ------------------------------------------------------------------------
void Receiver::userAccept(uint32_t token) {
    if (!_ready || _s.state != STATE_CONFIRMING || token != _s.token) return;
    const uint32_t t = now();
    // The hold can only start after the guard: an accept earlier than both
    // together did not come from a real press-and-hold.
    if (since(t, _s.tDialog) < T_BUTTON_GUARD_MS + T_HOLD_MS) return;
    // Things that may have changed since the last tick.
    if (!_env.host->acceptUpdates()) { closeDialog(true, RSN_DISABLED); return; }
    if (_env.host->alarmActive()) { closeDialog(true, RSN_ALARM_ACTIVE); return; }
    sendReply(_s.src, _s.sid, DECISION_ACCEPTED, 0, 0, (uint8_t)WINDOW_CHUNKS);
    enter(STATE_PREPARING);
}

void Receiver::userDecline(uint32_t token) {
    if (!_ready || _s.state != STATE_CONFIRMING || token != _s.token) return;
    closeDialog(false, RSN_DECLINED_BY_USER);
}

// The user tapped the identify screen away (7.7): this display has been found,
// so a sender that keeps identifying is ignored for a minute.
void Receiver::userIdentifyClosed() {
    if (!_ready) return;
    _identifySeen = true;
    _tIdentifyFree = now() + T_IDENTIFY_TAP_QUIET_MS;
}

void Receiver::userCancel(uint32_t token) {
    if (!_ready || token != _s.token) return;
    if (_s.state == STATE_CONFIRMING) { closeDialog(false, RSN_DECLINED_BY_USER); return; }
    if (_s.state >= STATE_PREPARING && _s.state <= STATE_READY_TO_APPLY)
        fail(RSN_USER_CANCELLED, FAIL_ABORT);
}

// ---- tick ---------------------------------------------------------------------------------
void Receiver::tick() {
    if (!_ready) return;
    const uint32_t t = now();
    const uint8_t st = _s.state;
    if (st == STATE_IDLE) return;
    IUpdHost &h = *_env.host;

    // Setting switched OFF (5.8, 5.9): every state up to READY_TO_APPLY ends.
    if (st >= STATE_CONFIRMING && st <= STATE_READY_TO_APPLY && !h.acceptUpdates()) {
        if (st == STATE_CONFIRMING) closeDialog(true, RSN_DISABLED);
        else fail(RSN_DISABLED, FAIL_ABORT);
        return;
    }

    // Keepalive STATUS in PREPARING, PROCESSING, VERIFYING, APPLYING (5.11).
    if ((st == STATE_PREPARING || st == STATE_PROCESSING || st == STATE_VERIFYING ||
         st == STATE_APPLYING) && since(t, _s.tKeepalive) >= T_KEEPALIVE_MS) {
        _s.tKeepalive = t;
        sendStatus(_s.src, _s.sid);
    }

    // Session idle (RECEIVING, PROCESSING): 60 s without the sender, counted
    // from the later of its last message and the state entry.
    if (st == STATE_RECEIVING || st == STATE_PROCESSING) {
        const uint32_t ref = since(t, _s.tHeard) < since(t, _s.tState) ? _s.tHeard : _s.tState;
        if (since(t, ref) >= T_SESSION_IDLE_MS) { fail(RSN_TIMEOUT, FAIL_ABORT); return; }
    }

    switch (st) {
        case STATE_CONFIRMING:
            if (h.alarmActive()) { closeDialog(true, RSN_ALARM_ACTIVE); return; }
            if (since(t, _s.tDialog) >= T_DIALOG_MS) { closeDialog(true, RSN_CONFIRM_TIMEOUT); return; }
            if (since(t, _s.tHeard) >= T_CONFIRM_SILENT_MS) { closeDialog(true, 0); return; }
            return;
        case STATE_PREPARING:
            if (since(t, _s.tState) >= T_PREPARING_MAX_MS) { fail(RSN_INTERNAL_ERROR, FAIL_ABORT); return; }
            doPreparing(t);
            return;
        case STATE_RECEIVING:
            // A4: no DATA for 1 s while the window is incomplete; repeated
            // every second, at most 5 times in a row.
            if (_s.a4Count < GAP_ACK_REPEATS &&
                since(t, _s.tGapRef) >= T_GAP_ACK_MS * (uint32_t)(_s.a4Count + 1)) {
                _s.a4Count++;
                sendAck();
            }
            return;
        case STATE_PROCESSING:
            doProcessing(t);
            return;
        case STATE_VERIFYING:
            doVerifying(t);
            return;
        case STATE_READY_TO_APPLY:
            if (since(t, _s.tState) >= T_APPLY_WAIT_MS) {
                // 5.6: no APPLY within 10 min - discard silently.
                toIdle();
                return;
            }
            break;
        case STATE_APPLYING:
            doApplying(t);
            return;
        case STATE_DONE:
            if (since(t, _s.tState) >= T_END_REPORT_MS) { toIdle(); return; }
            break;
        case STATE_FAILED:
            if (since(t, _s.tState) >= T_END_REPORT_MS) toIdle();
            return;
        default:
            return;
    }

    // RESULT repeat in READY_TO_APPLY and DONE (3.7).
    if (_s.haveResult && since(t, _s.tHeard) < T_RESULT_HEARD_MS &&
        since(t, _s.tResult) >= T_RESULT_REPEAT_MS)
        sendResult();
}

// 9.1: a unit starts only while the transmit path is idle, waiting for that
// at most 250 ms (a bus without ACKs might never drain), and not sooner than
// UPD_FLASH_GAP_MS after the previous unit.
bool Receiver::flashReady(uint32_t t) {
    if (_cfg.flashGapMs && _s.unitDone && since(t, _s.tUnitEnd) < _cfg.flashGapMs) return false;
    if (_env.link->txIdle()) { _s.txWaiting = false; return true; }
    if (!_s.txWaiting) { _s.txWaiting = true; _s.tTxWait = t; return false; }
    if (since(t, _s.tTxWait) >= T_TX_IDLE_MAX_MS) { _s.txWaiting = false; return true; }
    return false;
}

void Receiver::flashDone(uint32_t t) {
    _s.unitDone = true;
    _s.tUnitEnd = t;
    _stats.flashUnits++;
}

// PREPARING (9.1): wait for the work region, then begin the writer and hand
// out the first credit.
void Receiver::doPreparing(uint32_t t) {
    if (!_s.workHeld) {
        WorkArea wa;
        memset(&wa, 0, sizeof(wa));
        const uint32_t wait = _env.host->acquireWorkArea(wa);
        if (wait) { _s.workWaitMs = wait; return; }
        _s.workHeld = true;
        _s.work = wa;
        _s.workWaitMs = 0;
        if (!wa.window || (_s.offer.encoding == ENC_DEFLATE && !wa.dict)) {
            // OUT_OF_MEMORY is a REJECT reason only (4.8); an ABORT says this.
            fail(RSN_INTERNAL_ERROR, FAIL_ABORT);
            return;
        }
    }
    if (!flashReady(t)) return;
    IUpdTarget *tg = target();
    const uint8_t r = tg ? tg->begin(_s.offer.imageSize) : (uint8_t)RSN_INTERNAL_ERROR;
    flashDone(now());
    if (r) { fail(r, FAIL_ABORT); return; }
    _s.targetOpen = true;
    _env.sha->init();
    if (_s.offer.encoding == ENC_DEFLATE &&
        (!_env.inflater || !_env.inflater->init(_s.work.inflaterState))) {
        fail(RSN_INTERNAL_ERROR, FAIL_ABORT);
        return;
    }
    _s.crc = 0;
    _s.base = 0;
    _s.end = minU32(WINDOW_BYTES, _s.offer.streamSize);
    _s.bitmap = 0;
    _s.imageWritten = 0;
    _s.decoded = 0;
    _s.dictOut = _s.dictRd = _s.pending = 0;
    const uint32_t tc = now();
    enter(STATE_RECEIVING);
    _s.tGapRef = tc;
    _s.a4Count = 0;
    _s.transferStart = tc ? tc : 1;
    sendAck();                         // first credit
}

// Copies `n` image bytes through the bounce buffer: one SHA update and one
// target write per piece. Pieces end on 256-byte boundaries of the image
// offset, so a unit is at most 16 of them.
bool Receiver::writeBounced(const uint8_t *src, uint32_t n) {
    IUpdTarget *tg = target();
    while (n) {
        uint32_t piece = (uint32_t)BOUNCE_BYTES - (_s.imageWritten % (uint32_t)BOUNCE_BYTES);
        if (piece > n) piece = n;
        if ((uint64_t)_s.imageWritten + piece > _s.offer.imageSize) {
            fail(RSN_SIZE_MISMATCH, FAIL_ABORT);
            return false;
        }
        memcpy(_env.bounce, src, piece);
        _env.sha->update(_env.bounce, piece);
        const uint8_t r = tg->write(_s.imageWritten, _env.bounce, piece);
        if (r) { fail(r, FAIL_ABORT); return false; }
        _s.imageWritten += piece;
        src += piece;
        n -= piece;
    }
    return true;
}

bool Receiver::writeUnitRaw() {
    const uint32_t toBoundary = FLASH_UNIT - (_s.imageWritten % FLASH_UNIT);
    const uint32_t n = minU32(toBoundary, _s.procLen - _s.procPos);
    if (!writeBounced(_s.work.window + _s.procPos, n)) return false;
    _s.procPos += n;
    return true;
}

// 9.3: decode up to the next 4 KB boundary of the image offset. Decoded bytes
// wait in the dictionary ([dictRd, dictOut), "pending") until they are
// written; the decoder runs again only once nothing is pending, so it never
// overwrites bytes that have not reached the target.
bool Receiver::writeUnitDeflate() {
    const uint32_t toBoundary = FLASH_UNIT - (_s.imageWritten % FLASH_UNIT);
    const bool finalWindow = _s.base >= _s.offer.streamSize;
    uint32_t written = 0;
    for (int guard = 0; guard < 64; guard++) {
        if (_s.pending) {
            const uint32_t n = minU32(_s.pending, toBoundary - written);
            if (!writeBounced(_s.work.dict + _s.dictRd, n)) return false;
            _s.dictRd += n;
            _s.pending -= n;
            written += n;
            if (written >= toBoundary) return true;
            continue;
        }
        if (_s.inflDone || _s.inflEnded) return true;
        if (_s.dictOut >= DICT_BYTES) { _s.dictOut = 0; _s.dictRd = 0; }

        const size_t inAvail = _s.procLen - _s.procPos;
        const size_t outAvail = DICT_BYTES - _s.dictOut;
        size_t inBytes = inAvail;
        size_t outBytes = outAvail;
        const int st = _env.inflater->decompress(_s.work.window + _s.procPos, &inBytes,
                                                 _s.work.dict, _s.dictOut, &outBytes,
                                                 !finalWindow);
        if (inBytes > inAvail || outBytes > outAvail) { fail(RSN_INTERNAL_ERROR, FAIL_ABORT); return false; }
        _s.procPos += (uint32_t)inBytes;
        _s.dictRd = _s.dictOut;
        _s.dictOut += (uint32_t)outBytes;
        _s.pending = (uint32_t)outBytes;
        _s.decoded += outBytes;
        _s.inflStatus = st;
        // Output beyond the image size: SIZE_MISMATCH at once.
        if (_s.decoded > _s.offer.imageSize) { fail(RSN_SIZE_MISMATCH, FAIL_ABORT); return false; }

        const bool inputGone = _s.procPos >= _s.procLen;
        if (st == INFL_DONE) {
            _s.inflDone = true;
            // DONE in a window that is not the last: the stream goes on.
            if (!finalWindow) { fail(RSN_SIZE_MISMATCH, FAIL_ABORT); return false; }
            continue;                  // flush; leftover bytes are step 2 of 5.5
        }
        if (st == INFL_NEEDS_MORE_INPUT && inputGone) { _s.inflEnded = true; continue; }
        if (st == INFL_CANNOT_PROGRESS && finalWindow && inputGone) {
            _s.inflEnded = true;       // truncated stream, newer miniz: step 2 of 5.5
            continue;
        }
        if (st == INFL_HAS_MORE_OUTPUT || st == INFL_NEEDS_MORE_INPUT) {
            if (inBytes == 0 && outBytes == 0) { fail(RSN_IMAGE_INVALID, FAIL_ABORT); return false; }
            continue;
        }
        fail(RSN_IMAGE_INVALID, FAIL_ABORT);   // a negative status: corrupt stream
        return false;
    }
    return true;
}

bool Receiver::windowFinished() const {
    if (_s.offer.encoding == ENC_DEFLATE)
        return _s.pending == 0 && (_s.inflDone || _s.inflEnded);
    return _s.procPos >= _s.procLen;
}

// A2, or VERIFYING once base reached the stream size.
void Receiver::finishWindow(uint32_t t) {
    if (_s.base >= _s.offer.streamSize) {
        _s.vStep = V_CHECKS;
        enter(STATE_VERIFYING);
        sendStatus(_s.src, _s.sid);
        return;
    }
    _s.end = minU32(_s.base + WINDOW_BYTES, _s.offer.streamSize);
    _s.bitmap = 0;
    _s.procLen = _s.procPos = 0;
    _s.inflEnded = false;
    enter(STATE_RECEIVING);
    _s.tGapRef = t;
    _s.a4Count = 0;
    sendAck();
}

// 8.2: the identity check from flash. Reads image bytes [0, 0xB8) and the
// descriptor at 0x120 back through the bounce buffer. Returns a reason.
static uint8_t identityFromTarget(IUpdTarget *tg, uint8_t *bounce, uint32_t imageSize,
                                  uint32_t written, const DescExpect &e, uint8_t imageId[8]) {
    if (imageSize < FWDESC_CHECK_BYTES || written < FWDESC_CHECK_BYTES) return RSN_DESCRIPTOR_INVALID;
    uint8_t head[APP_DESC_OFFSET + 4];
    uint8_t r = tg->readBack(0, bounce, IMAGE_ID_OFFSET + IMAGE_ID_LEN);
    if (r) return RSN_FLASH_ERROR;
    memcpy(head, bounce, sizeof(head));
    memcpy(imageId, bounce + IMAGE_ID_OFFSET, IMAGE_ID_LEN);
    r = tg->readBack(FWDESC_OFFSET, bounce, FWDESC_SIZE);
    if (r) return RSN_FLASH_ERROR;
    return checkImageIdentity(head, sizeof(head), bounce, FWDESC_SIZE, e);
}

bool Receiver::descriptorCheck() {
    DescExpect e;
    e.boardId = _id.boardId;
    e.panelW = _id.panelW;
    e.panelH = _id.panelH;
    e.version16 = _s.offer.fwVersion;
    const uint8_t r = identityFromTarget(target(), _env.bounce, _s.offer.imageSize,
                                         _s.imageWritten, e, _s.imageId);
    _s.descDue = false;
    if (r) { fail(r, FAIL_ABORT); return false; }
    _s.descChecked = true;
    return true;
}

// PROCESSING: one unit per tick, then A2 when the window is written.
void Receiver::doProcessing(uint32_t t) {
    if (!flashReady(t)) return;
    if (_s.descDue) {
        const bool ok = descriptorCheck();
        flashDone(now());
        if (!ok) return;
    } else {
        const bool ok = _s.offer.encoding == ENC_DEFLATE ? writeUnitDeflate() : writeUnitRaw();
        flashDone(now());
        if (!ok) return;
        // 8.2: as soon as 0x160 bytes are in flash, before anything else.
        if (_s.offer.target == TARGET_FIRMWARE && !_s.descChecked &&
            _s.imageWritten >= FWDESC_CHECK_BYTES) {
            _s.descDue = true;
            return;
        }
    }
    if (windowFinished()) finishWindow(now());
}

// 5.5, one step per tick; every step that touches flash waits for TX idle.
void Receiver::doVerifying(uint32_t t) {
    const OfferMsg &o = _s.offer;
    IUpdTarget *tg = target();
    uint8_t r = RSN_OK;
    switch (_s.vStep) {
        case V_CHECKS: {
            // 1. the whole-stream CRC
            if (_s.crc != o.streamCrc) r = RSN_STREAM_CRC_MISMATCH;
            // 2. the decoder finished with exactly image size bytes, nothing left over
            else if (_s.imageWritten != o.imageSize) r = RSN_SIZE_MISMATCH;
            else if (o.encoding == ENC_DEFLATE &&
                     (!_s.inflDone || _s.procPos != _s.procLen || _s.decoded != o.imageSize))
                r = RSN_SIZE_MISMATCH;
            // 3. the write-time SHA-256, the quick path without a read-back
            else {
                uint8_t h[32];
                _env.sha->finish(h);
                if (memcmp(h, o.sha256, 32) != 0) r = RSN_SHA_MISMATCH;
            }
            if (r) break;
            _s.vStep = V_FINISH;
            return;
        }
        case V_FINISH:
            // 4. esp_ota_end (image structure, appended hash) / close the file
            if (!flashReady(t)) return;
            r = tg->finish();
            flashDone(now());
            if (r) break;
            _env.sha->init();
            _s.readPos = 0;
            _s.vStep = V_READBACK;
            return;
        case V_READBACK: {
            // 5. read back through the bounce buffer; only this value counts
            if (!flashReady(t)) return;
            const uint32_t stop = minU32(o.imageSize, (_s.readPos / FLASH_UNIT + 1) * FLASH_UNIT);
            while (_s.readPos < stop) {
                const uint32_t n = minU32((uint32_t)BOUNCE_BYTES, stop - _s.readPos);
                r = tg->readBack(_s.readPos, _env.bounce, n);
                if (r) { r = RSN_FLASH_ERROR; break; }
                _env.sha->update(_env.bounce, n);
                _s.readPos += n;
            }
            flashDone(now());
            if (r) break;
            if (_s.readPos < o.imageSize) return;
            _env.sha->finish(_s.resSha);
            _s.haveResult = false;
            if (memcmp(_s.resSha, o.sha256, 32) != 0) {
                storeResult(RSN_SHA_MISMATCH, PHASE_VERIFIED, 0, _s.resSha);
                sendResult();
                fail(RSN_SHA_MISMATCH, FAIL_RESULT);
                return;
            }
            _s.vStep = V_IDENTITY;
            return;
        }
        case V_IDENTITY: {
            // 6. firmware: the descriptor again, from flash; web UI: gzip magic
            if (!flashReady(t)) return;
            if (o.target == TARGET_FIRMWARE) {
                DescExpect e;
                e.boardId = _id.boardId;
                e.panelW = _id.panelW;
                e.panelH = _id.panelH;
                e.version16 = o.fwVersion;
                r = identityFromTarget(tg, _env.bounce, o.imageSize, _s.imageWritten, e, _s.imageId);
            } else if (o.imageSize < sizeof(GZIP_MAGIC)) {
                r = RSN_IMAGE_INVALID;         // too short to be gzip; nothing to read back
            } else {
                r = tg->readBack(0, _env.bounce, 3) ? (uint8_t)RSN_FLASH_ERROR
                    : (memcmp(_env.bounce, GZIP_MAGIC, 3) == 0 ? (uint8_t)RSN_OK
                                                               : (uint8_t)RSN_IMAGE_INVALID);
            }
            flashDone(now());
            if (r) {
                storeResult(r, PHASE_VERIFIED, 0, _s.resSha);
                sendResult();
                fail(r, FAIL_RESULT);
                return;
            }
            // 7. RESULT(OK, phase 1, read-back SHA)
            storeResult(RSN_OK, PHASE_VERIFIED, 0, _s.resSha);
            enter(STATE_READY_TO_APPLY);
            sendResult();
            return;
        }
        default:
            r = RSN_INTERNAL_ERROR;
            break;
    }
    // Failures of steps 1-5 before a read-back SHA exists: all-zero SHA.
    storeResult(r, PHASE_VERIFIED, 0, nullptr);
    sendResult();
    fail(r, FAIL_RESULT);
}

// 5.6: one target step per tick; the rename is retried every 100 ms for up
// to 10 s while it reports BUSY (9.4.4).
void Receiver::doApplying(uint32_t t) {
    if (_s.applyBusy && since(t, _s.tApplyTry) < T_RENAME_RETRY_MS) return;
    if (!flashReady(t)) return;
    const bool fw = _s.offer.target == TARGET_FIRMWARE;
    ApplyInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.target = _s.offer.target;
    ai.mode = _s.applyMode;
    memcpy(ai.imageId, _s.imageId, 8);
    memcpy(ai.version, _s.offer.fwVersion, 16);
    memcpy(ai.sha256, _s.resSha, 32);
    ai.imageSize = _s.offer.imageSize;
    if (companionSize()) {
        memcpy(ai.companionSha, _s.offer.companionSha, 32);
        ai.companionSize = _s.offer.companionSize;
    }
    ai.viaCompanion = _s.viaCompanion;
    uint8_t detail = 0;
    const ApplyStatus as = target()->apply(ai, &detail);
    const uint32_t tAfter = now();
    flashDone(tAfter);
    _s.tApplyTry = t;
    const uint8_t phase = fw ? (uint8_t)PHASE_ACTIVATED : (uint8_t)PHASE_APPLIED;
    switch (as) {
        case APPLY_ST_DONE: {
            // Phase 3 detail is the mode's meaning (3.7); phase 2 is the target's.
            const uint8_t d = fw ? (uint8_t)(_s.applyMode == APPLY_NEXT_RESTART ? 1 : 0)
                                 : (uint8_t)(detail ? 1 : 0);
            storeResult(RSN_OK, phase, d, _s.resSha);
            _s.targetOpen = false;     // nothing left to abort
            releaseWork();
            const uint32_t token = _s.token;
            const uint8_t mode = _s.applyMode;
            enter(STATE_DONE);
            sendResult();
            _env.ui->applied(token, _s.offer.target, mode, d);
            return;
        }
        case APPLY_ST_AGAIN:
            _s.applyBusy = false;
            return;
        case APPLY_ST_BUSY:
            if (!_s.applyBusy) { _s.applyBusy = true; _s.tApplyBusy = t; return; }
            if (since(t, _s.tApplyBusy) < T_RENAME_MAX_MS) return;
            break;
        default:
            break;
    }
    storeResult(RSN_APPLY_FAILED, phase, 0, _s.resSha);
    sendResult();
    fail(RSN_APPLY_FAILED, FAIL_RESULT);
}

// ---- boot self-tests 2-4 (8.1) ---------------------------------------------------------------
int runSelfTests(Crc32Fn crcFn, ISha256 &sha, IInflater *inflater, void *inflaterState,
                 uint8_t *dict, uint8_t *bounce) {
    if (!bounce) return 2;
    const Crc32Fn c = crcFn ? crcFn : crc32Update;
    memcpy(bounce, "123456789", 9);
    if (c(0, bounce, 9) != 0xCBF43926UL) return 2;

    static const uint8_t ABC_SHA[32] = {
        0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA, 0x41, 0x41, 0x40, 0xDE,
        0x5D, 0xAE, 0x22, 0x23, 0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17, 0x7A, 0x9C,
        0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD };
    uint8_t h[32];
    memcpy(bounce, "abc", 3);
    sha.init();
    sha.update(bounce, 3);
    sha.finish(h);
    if (memcmp(h, ABC_SHA, 32) != 0) return 3;

    if (inflater) {
        // Appendix A: raw deflate of "NauticPinnace inflate self-test " x 2.
        static const uint8_t VEC[37] = {
            0xF3, 0x4B, 0x2C, 0x2D, 0xC9, 0x4C, 0x0E, 0xC8, 0xCC, 0xCB, 0x4B, 0x4C,
            0x4E, 0x55, 0xC8, 0xCC, 0x4B, 0xCB, 0x49, 0x2C, 0x49, 0x55, 0x28, 0x4E,
            0xCD, 0x49, 0xD3, 0x2D, 0x49, 0x2D, 0x2E, 0x51, 0xF0, 0x23, 0x20, 0x0F,
            0x00 };
        static const char PLAIN[] = "NauticPinnace inflate self-test ";
        if (!dict || !inflater->init(inflaterState)) return 4;
        memcpy(bounce, VEC, sizeof(VEC));
        size_t in = sizeof(VEC);
        size_t out = DICT_BYTES;
        const int r = inflater->decompress(bounce, &in, dict, 0, &out, false);
        if (r != INFL_DONE || in != sizeof(VEC) || out != 64) return 4;
        if (memcmp(dict, PLAIN, 32) != 0 || memcmp(dict + 32, PLAIN, 32) != 0) return 4;
    }
    return 0;
}

}  // namespace n2kupd
