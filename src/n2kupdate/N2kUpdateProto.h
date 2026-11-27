#pragma once
// ============================================================================
// N2kUpdateProto - wire format of "updates over NMEA 2000", protocol version 1
// (docs/N2K-UPDATE.md, revision 2). THE ONE PLACE every protocol constant is
// written down: PGN, header, message types, lengths, code tables, timers.
//
// Pure C++11 on purpose: stdint, stddef and string.h, nothing else. No Arduino,
// no IDF, no LVGL, no NMEA2000 library. That is what lets the same code run on
// the device, in the PC simulator and in the host tests against the normative
// vectors of Appendix A.
//
// CARRIER. Addressed proprietary fast-packet PGN 126720, manufacturer 2046,
// industry group 4. Every message starts with the manufacturer header FE 9F,
// then a type byte 0xA0..0xAA. Control messages (all but DATA) carry the
// protocol version in byte 3 and the session id in byte 4.
//
// THE HEADER IS NOT A SIGNATURE. 2046 is the NMEA2000 library's default code, chosen
// as a free code from the NMEA list, not assigned by the NMEA. A message is ours
// only if PGN, header, a known type, the version byte where there is one, the exact
// minimum length and, for session messages, the session binding hold (2.2); none is redundant.
//
// All integers are little endian. A later minor revision may only APPEND
// fields; parsers accept longer messages and ignore the extra bytes (3.0).
// ============================================================================
#include <stdint.h>
#include <stddef.h>

namespace n2kupd {

// ---- carrier (2.2) ------------------------------------------------------------
static constexpr uint32_t PGN               = 126720UL;
static constexpr uint16_t MFG_CODE          = 2046;
// (4 << 13) | (3 << 11) | 2046, little endian on the wire: FE 9F.
static constexpr uint8_t  HDR0              = 0xFE;
static constexpr uint8_t  HDR1              = 0x9F;
static constexpr uint8_t  PRIO_CONTROL      = 6;
static constexpr uint8_t  PRIO_DATA         = 7;
static constexpr uint8_t  ADDR_BROADCAST    = 255;
static constexpr size_t   MAX_PAYLOAD       = 223;   // 32 fast-packet frames

// ---- protocol version (3.0, 13) -------------------------------------------------
static constexpr uint8_t  PROTO_VERSION     = 1;
static constexpr uint8_t  PROTO_MIN         = 1;
static constexpr uint8_t  PROTO_MAX         = 1;

// ---- transfer geometry (3.3, 5.4, 9.1) ----------------------------------------
static constexpr uint32_t CHUNK_SIZE        = 216;   // fixed in v1
static constexpr uint32_t WINDOW_CHUNKS     = 64;
static constexpr uint32_t WINDOW_BYTES      = CHUNK_SIZE * WINDOW_CHUNKS;  // 13,824
static constexpr uint32_t DICT_BYTES        = 32768; // deflate dictionary (TINFL_LZ_DICT_SIZE)
static constexpr size_t   BOUNCE_BYTES      = 256;   // the one internal-RAM bounce buffer
static constexpr uint32_t FLASH_UNIT        = 4096;  // one sector = one unit of flash work
static constexpr uint32_t MAX_STREAM_SIZE   = 0xFFFFFFUL;  // u24 offsets
static constexpr uint32_t DATA_HDR_LEN      = 7;
static constexpr uint32_t WEBUI_FREE_MARGIN = 65536; // LittleFS space kept free (5.3.1/9)

// ---- message types (2.5) -----------------------------------------------------
enum MsgType : uint8_t {
    MSG_QUERY       = 0xA0,   // sender -> display (or 255)
    MSG_INFO        = 0xA1,   // display -> sender
    MSG_OFFER       = 0xA2,   // sender -> display
    MSG_OFFER_REPLY = 0xA3,   // display -> sender
    MSG_DATA        = 0xA4,   // sender -> display
    MSG_ACK         = 0xA5,   // display -> sender
    MSG_RESULT      = 0xA6,   // display -> sender
    MSG_APPLY       = 0xA7,   // sender -> display
    MSG_ABORT       = 0xA8,   // both directions
    MSG_STATUS      = 0xA9,   // display -> sender
    MSG_IDENTIFY    = 0xAA,   // sender -> display
};
static constexpr uint8_t MSG_FIRST = MSG_QUERY;
static constexpr uint8_t MSG_LAST  = MSG_IDENTIFY;

// v1 lengths. Shorter messages are ignored; longer ones are accepted (3.0).
static constexpr size_t LEN_QUERY       = 12;
static constexpr size_t LEN_INFO        = 129;
static constexpr size_t LEN_OFFER       = 131;
static constexpr size_t LEN_OFFER_REPLY = 10;
static constexpr size_t LEN_DATA_MIN    = DATA_HDR_LEN + 1;
static constexpr size_t LEN_DATA_FULL   = DATA_HDR_LEN + CHUNK_SIZE;   // 223
static constexpr size_t LEN_ACK         = 28;
static constexpr size_t LEN_RESULT      = 40;
static constexpr size_t LEN_APPLY       = 6;
static constexpr size_t LEN_ABORT       = 6;
static constexpr size_t LEN_STATUS      = 25;
static constexpr size_t LEN_IDENTIFY    = 12;
static constexpr size_t LEN_CTRL_HDR    = 5;

// ---- code tables (4.x) ---------------------------------------------------------
enum BoardId : uint8_t {          // 4.1 - only ever added, never renumbered
    BOARD_ANY = 0, BOARD_4INCH = 1, BOARD_7B = 2, BOARD_5B = 3,
};
enum Target : uint8_t {           // 4.2
    TARGET_FIRMWARE = 1, TARGET_WEBUI = 2,
};
enum Encoding : uint8_t {         // 4.3
    ENC_RAW = 0, ENC_DEFLATE = 1,
};
enum Capability : uint8_t {       // 4.4, INFO byte 8
    CAP_FIRMWARE = 0x01, CAP_WEBUI = 0x02, CAP_DEFLATE = 0x04,
    CAP_RESUME = 0x08, CAP_IDENTIFY = 0x10, CAP_COMPANION = 0x20,
};
enum StatusFlag : uint8_t {       // 4.5, INFO byte 20
    SF_ACCEPT_UPDATES = 0x01, SF_ALARM = 0x02, SF_RESTART_PENDING = 0x04,
    SF_ROLLED_BACK = 0x08, SF_READY = 0x10, SF_COMPANION_WAITING = 0x20,
    SF_WIFI_OTA = 0x40,
};
enum OtaState : uint8_t {         // 4.6
    OTA_NA = 0, OTA_VALID = 1, OTA_PENDING_VERIFY = 2,
};
enum State : uint8_t {            // 4.7
    STATE_IDLE = 0, STATE_CONFIRMING = 1, STATE_PREPARING = 2,
    STATE_RECEIVING = 3, STATE_PROCESSING = 4, STATE_VERIFYING = 5,
    STATE_READY_TO_APPLY = 6, STATE_APPLYING = 7, STATE_DONE = 8,
    STATE_FAILED = 9,
};
enum Reason : uint8_t {           // 4.8
    RSN_OK                   = 0x00,
    RSN_DECLINED_BY_USER     = 0x01,
    RSN_CONFIRM_TIMEOUT      = 0x02,
    RSN_DISABLED             = 0x03,
    RSN_BUSY                 = 0x04,
    RSN_WRONG_BOARD          = 0x05,
    RSN_TOO_LARGE            = 0x06,
    RSN_UNSUPPORTED_TARGET   = 0x07,
    RSN_UNSUPPORTED_ENCODING = 0x08,
    RSN_UNSUPPORTED_PROTOCOL = 0x09,
    RSN_BAD_REQUEST          = 0x0A,
    RSN_PENDING_VERIFY       = 0x0B,
    RSN_ALARM_ACTIVE         = 0x0C,
    RSN_WRONG_DEVICE         = 0x0D,
    RSN_VERSION_MISMATCH     = 0x0E,
    RSN_NO_SPACE             = 0x0F,
    RSN_OUT_OF_MEMORY        = 0x10,
    RSN_DESCRIPTOR_INVALID   = 0x11,
    RSN_SHA_MISMATCH         = 0x12,
    RSN_IMAGE_INVALID        = 0x13,
    RSN_SIZE_MISMATCH        = 0x14,
    RSN_FLASH_ERROR          = 0x15,
    RSN_TIMEOUT              = 0x16,
    RSN_USER_CANCELLED       = 0x17,
    RSN_SENDER_CANCELLED     = 0x18,
    RSN_SESSION_UNKNOWN      = 0x19,
    RSN_STREAM_CRC_MISMATCH  = 0x1A,
    RSN_LINK_UNRELIABLE      = 0x1B,
    RSN_APPLY_FAILED         = 0x1C,
    RSN_RESTART_PENDING      = 0x1D,
    RSN_NOT_READY            = 0x1E,
    RSN_LOW_VOLTAGE          = 0x1F,   // reserved, never sent in v1 (9.6)
    RSN_INTERNAL_ERROR       = 0xFE,
};
enum SenderKind : uint8_t {       // 4.9
    SENDER_OTHER = 0, SENDER_NAUTICPI = 1, SENDER_UPDATER = 2,
};
enum OfferFlag : uint8_t {        // 4.10
    OFFER_F_RESUME = 0x01, OFFER_F_ALLOW_VERSION_MISMATCH = 0x02,
};
enum RollbackCause : uint8_t {    // 4.11
    RB_NONE = 0, RB_HEALTH = 1, RB_CRASH = 2, RB_POWER = 3, RB_RESTART = 4,
    RB_CRASH_LOOP = 5,
};
enum QueryWhat : uint8_t {        // 4.12
    QUERY_INFO = 1, QUERY_STATUS = 2, QUERY_ACK = 3, QUERY_RESULT = 4,
};
enum Decision : uint8_t {         // 3.4
    DECISION_PENDING_USER = 0, DECISION_ACCEPTED = 1, DECISION_REJECTED = 2,
};
enum Phase : uint8_t {            // 3.7
    PHASE_VERIFIED = 1, PHASE_APPLIED = 2, PHASE_ACTIVATED = 3,
};
enum ApplyMode : uint8_t {        // 3.8
    APPLY_NOW = 0,                // countdown, sender polls INFO (H5 PROTOCOL)
    APPLY_NEXT_RESTART = 1,       // activate, no restart (H5 ANY_FRAME)
    APPLY_NOW_NO_POLL = 2,        // countdown, sender does not poll (H5 ANY_FRAME)
};

// ---- display timers, normative values for v1 (5.11) --------------------------
static constexpr uint32_t T_DIALOG_MS            = 60000;
static constexpr uint32_t T_BUTTON_GUARD_MS      = 1500;
static constexpr uint32_t T_HOLD_MS              = 1500;
static constexpr uint32_t T_CONFIRM_SILENT_MS    = 15000;
static constexpr uint32_t T_GAP_ACK_MS           = 1000;   // A4
static constexpr uint8_t  GAP_ACK_REPEATS        = 5;
static constexpr uint32_t T_SESSION_IDLE_MS      = 60000;
static constexpr uint32_t T_PREEMPT_SILENCE_MS   = 5000;
static constexpr uint32_t T_KEEPALIVE_MS         = 1000;
static constexpr uint32_t T_RESULT_REPEAT_MS     = 5000;
static constexpr uint32_t T_RESULT_HEARD_MS      = 60000;
static constexpr uint32_t T_TX_IDLE_MAX_MS       = 250;
static constexpr uint32_t T_APPLY_WAIT_MS        = 600000;
static constexpr uint32_t T_END_REPORT_MS        = 30000;  // DONE/FAILED, then IDLE
static constexpr uint32_t T_REGION_GRACE_MS      = 120000;
static constexpr uint32_t T_COMPANION_USABLE_MS  = 1800000;
static constexpr uint32_t T_IDENTIFY_INTERVAL_MS = 10000;
static constexpr uint8_t  IDENTIFY_MAX_S         = 30;
static constexpr uint32_t T_RESTART_COUNTDOWN_MS = 10000;
static constexpr uint32_t T_A6_INTERVAL_MS       = 250;
static constexpr uint8_t  A6_BELOW_BASE_COUNT    = 3;
static constexpr uint32_t T_SESSION_UNKNOWN_MS   = 1000;
static constexpr uint8_t  MAX_MALFORMED_DATA     = 20;
static constexpr uint32_t T_RENAME_RETRY_MS      = 100;    // 9.4.4
static constexpr uint32_t T_RENAME_MAX_MS        = 10000;

// ---- image identity (8.1-8.3) --------------------------------------------------
static constexpr uint8_t  IMAGE_MAGIC        = 0xE9;
static constexpr uint32_t APP_DESC_OFFSET    = 0x20;
static constexpr uint32_t APP_DESC_MAGIC     = 0xABCD5432UL;
static constexpr uint32_t IMAGE_ID_OFFSET    = 0xB0;   // app_elf_sha256[0..7]
static constexpr uint32_t IMAGE_ID_LEN       = 8;
static constexpr uint32_t FWDESC_OFFSET      = 0x120;
static constexpr uint32_t FWDESC_SIZE        = 64;
static constexpr uint32_t FWDESC_CHECK_BYTES = FWDESC_OFFSET + FWDESC_SIZE;  // 0x160
static constexpr uint8_t  FWDESC_F_OTAGUARD  = 0x01;
// The descriptor magic is "NPFWDESC"; see isDescriptorMagic().

// ---- message structs ------------------------------------------------------------
// Text fields are kept as the raw 16 bytes of the wire (str[16] or UTF-8,
// NUL-padded). Use the field helpers below to fill or check them.
struct QueryMsg {
    uint8_t version, session, what;
    uint8_t uidFilter[6];        // FF x 6 = any display
};

struct InfoMsg {
    uint8_t  version, session;
    uint8_t  protoMin, protoMax, boardId, capabilities;
    uint8_t  uniqueId[6];
    uint8_t  sessionState, otaState;
    uint16_t verifySecondsLeft;  // 0xFFFF = not verifying
    uint8_t  runningSlot;        // 0 / 1 / 0xFF
    uint8_t  statusFlags;
    uint32_t maxFirmwareSize, maxWebUiSize;
    uint8_t  maxChunkSize, maxWindowChunks;
    uint8_t  runningVersion[16];
    uint8_t  lastInvalidVersion[16];
    uint8_t  label[16];
    uint8_t  runningImageId[8];
    uint8_t  lastInvalidImageId[8];
    uint8_t  rollbackCause, rollbackDetail;
    uint8_t  webUiVersion[16];
    uint8_t  webUiId[8];
    uint32_t webUiSize;
    uint32_t uptimeS;
};

struct OfferMsg {
    uint8_t  version, session;
    uint8_t  target, boardId, encoding, flags, chunkSize;
    uint32_t imageSize, streamSize;
    uint8_t  sha256[32];
    uint8_t  targetUid[6];
    uint8_t  fwVersion[16];
    uint8_t  senderName[16];
    uint8_t  senderKind;
    uint16_t estSeconds;         // 0xFFFF = unknown
    uint32_t streamCrc;
    uint8_t  companionSha[32];
    uint32_t companionSize;
};

struct OfferReplyMsg {
    uint8_t  version, session, decision, reason;
    uint16_t seconds;
    uint8_t  windowChunks;
};

struct DataMsg {                 // parse result points INTO the received payload
    uint8_t        session;
    uint32_t       offset;
    const uint8_t *data;
    uint16_t       len;
};

struct AckMsg {
    uint8_t  version, session, state;
    uint32_t base, end;
    uint64_t bitmap;
    uint32_t crc;
    uint16_t holdMs;
};

struct ResultMsg {
    uint8_t version, session, code, phase, detail;
    uint8_t sha256[32];
};

struct ApplyMsg    { uint8_t version, session, mode; };
struct AbortMsg    { uint8_t version, session, reason; };

struct StatusMsg {
    uint8_t  version, session, state, reason;
    uint32_t streamAccepted, streamSize, imageWritten, imageSize;
    uint16_t seconds;            // 0xFFFF = NA
};

struct IdentifyMsg {
    uint8_t version, session, seconds;
    uint8_t targetUid[6];
};

// ---- codec ------------------------------------------------------------------------
// Returns the message type (0xA0..0xAA) if `p` starts with FE 9F and a known
// type, else 0. Says nothing about length or version.
uint8_t msgType(const uint8_t *p, size_t len);
// The v1 minimum length of a type (DATA: 8), 0 for an unknown type.
size_t minLength(uint8_t type);
// true for the types a display receives (QUERY, OFFER, DATA, APPLY, ABORT, IDENTIFY).
bool isSenderToDisplay(uint8_t type);

// Builders write the v1 layout with PROTO_VERSION and return the length
// written, or 0 if `cap` is too small. The `version` member is ignored.
size_t buildQuery(const QueryMsg &m, uint8_t *out, size_t cap);
size_t buildInfo(const InfoMsg &m, uint8_t *out, size_t cap);
size_t buildOffer(const OfferMsg &m, uint8_t *out, size_t cap);
size_t buildOfferReply(const OfferReplyMsg &m, uint8_t *out, size_t cap);
size_t buildData(uint8_t session, uint32_t offset, const uint8_t *data, size_t n,
                 uint8_t *out, size_t cap);
size_t buildAck(const AckMsg &m, uint8_t *out, size_t cap);
size_t buildResult(const ResultMsg &m, uint8_t *out, size_t cap);
size_t buildApply(const ApplyMsg &m, uint8_t *out, size_t cap);
size_t buildAbort(const AbortMsg &m, uint8_t *out, size_t cap);
size_t buildStatus(const StatusMsg &m, uint8_t *out, size_t cap);
size_t buildIdentify(const IdentifyMsg &m, uint8_t *out, size_t cap);

// Parsers check header, type and the v1 minimum length and return false
// otherwise (the output is then untouched). They do NOT check the protocol
// version: that is policy (5.1 rule 3), and the value is returned in `version`.
bool parseQuery(const uint8_t *p, size_t len, QueryMsg &m);
bool parseInfo(const uint8_t *p, size_t len, InfoMsg &m);
bool parseOffer(const uint8_t *p, size_t len, OfferMsg &m);
bool parseOfferReply(const uint8_t *p, size_t len, OfferReplyMsg &m);
bool parseData(const uint8_t *p, size_t len, DataMsg &m);   // n >= 1
bool parseAck(const uint8_t *p, size_t len, AckMsg &m);
bool parseResult(const uint8_t *p, size_t len, ResultMsg &m);
bool parseApply(const uint8_t *p, size_t len, ApplyMsg &m);
bool parseAbort(const uint8_t *p, size_t len, AbortMsg &m);
bool parseStatus(const uint8_t *p, size_t len, StatusMsg &m);
bool parseIdentify(const uint8_t *p, size_t len, IdentifyMsg &m);

// ---- CRC-32/ISO-HDLC (8.5) ------------------------------------------------------
// Same contract as zlib crc32(prev, ...) and esp_rom_crc32_le(prev, ...):
// start with prev = 0, chain by passing the previous result.
uint32_t crc32Update(uint32_t prev, const uint8_t *p, size_t n);
typedef uint32_t (*Crc32Fn)(uint32_t prev, const uint8_t *p, size_t n);

// ---- field helpers (3.0) ---------------------------------------------------------
// str[16] version: 1-15 printable ASCII (0x20-0x7E), NUL-terminated, NUL-padded.
bool isValidVersionField(const uint8_t f[16]);
// Copies up to 15 characters of `s` and NUL-pads; returns false if `s` was
// longer or contained a character outside 0x20-0x7E (the field is still filled).
bool putVersionField(uint8_t out[16], const char *s);
// UTF-8 label / sender name: at most 16 bytes, cut at a character boundary,
// NUL-padded, not necessarily NUL-terminated when full.
void putUtf8Field(uint8_t out[16], const char *s);
bool isAllZero(const uint8_t *p, size_t n);
bool isAllFF(const uint8_t *p, size_t n);

// Turns a received text field into something the display fonts can draw:
// ASCII 0x20-0x7E and the glyphs listed in 3.0 are kept, everything else
// (other code points, control characters, broken UTF-8) becomes '?'. Stops at
// the first NUL or after `inLen` bytes. `out` is always NUL-terminated; the
// return value is the output length in bytes.
size_t sanitizeText(const uint8_t *in, size_t inLen, char *out, size_t outCap);

// Compares dotted versions numerically ("1.10.0" > "1.9.2"). Returns -1 when
// a < b, 0 when equal, 1 when a > b, and 2 when either is not a version that
// starts with a digit (the dialog then appends nothing).
int compareVersions(const char *a, const char *b);

// ---- firmware descriptor np_fw_desc_t (8.1, 8.2) ----------------------------------
struct FwDesc {
    uint8_t  descVersion, boardId;
    uint16_t descSize, panelW, panelH;
    uint8_t  n2kProto, flags;
    uint8_t  fwVersion[16];
    uint8_t  boardName[28];
};
bool isDescriptorMagic(const uint8_t *p);              // 8 bytes "NPFWDESC"
// Parses the 64 bytes at image offset 0x120; false without the magic.
bool parseDescriptor(const uint8_t *p, size_t len, FwDesc &d);
// Writes the 64-byte descriptor (for tests and tools).
size_t buildDescriptor(const FwDesc &d, uint8_t *out, size_t cap);

struct DescExpect {
    uint8_t        boardId;
    uint16_t       panelW, panelH;
    const uint8_t *version16;    // the OFFER's version field; nullptr = not checked
};
// The display-side identity check of 8.2, shared by the N2K session and WiFi
// OTA. `head` holds image bytes [0, headLen) with headLen >= 0x24; `desc` the
// FWDESC_SIZE bytes read back from image offset 0x120. Returns RSN_OK,
// RSN_IMAGE_INVALID (no 0xE9), RSN_WRONG_BOARD (board id) or
// RSN_DESCRIPTOR_INVALID (everything else, legacy images included).
uint8_t checkImageIdentity(const uint8_t *head, size_t headLen,
                           const uint8_t *desc, size_t descLen,
                           const DescExpect &expect);

// ---- little endian helpers ---------------------------------------------------------
// Written out by hand: the wire order must not depend on the compiler.
inline void putU16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8);
}
inline void putU24(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
}
inline void putU32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);         p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}
inline void putU64(uint8_t *p, uint64_t v) {
    putU32(p, (uint32_t)(v & 0xFFFFFFFFUL)); putU32(p + 4, (uint32_t)(v >> 32));
}
inline uint16_t getU16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
inline uint32_t getU24(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
inline uint32_t getU32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint64_t getU64(const uint8_t *p) {
    return (uint64_t)getU32(p) | ((uint64_t)getU32(p + 4) << 32);
}

}  // namespace n2kupd
