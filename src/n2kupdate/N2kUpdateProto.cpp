// ============================================================================
// N2kUpdateProto.cpp - codec of the update protocol; see N2kUpdateProto.h and
// docs/N2K-UPDATE.md section 3 for the layouts. Checked byte for byte against
// Appendix A by the host tests.
// ============================================================================
#include "N2kUpdateProto.h"
#include <string.h>

namespace n2kupd {

// ---- header -----------------------------------------------------------------------
uint8_t msgType(const uint8_t *p, size_t len) {
    if (!p || len < 3 || p[0] != HDR0 || p[1] != HDR1) return 0;
    if (p[2] < MSG_FIRST || p[2] > MSG_LAST) return 0;
    return p[2];
}

size_t minLength(uint8_t type) {
    switch (type) {
        case MSG_QUERY:       return LEN_QUERY;
        case MSG_INFO:        return LEN_INFO;
        case MSG_OFFER:       return LEN_OFFER;
        case MSG_OFFER_REPLY: return LEN_OFFER_REPLY;
        case MSG_DATA:        return LEN_DATA_MIN;
        case MSG_ACK:         return LEN_ACK;
        case MSG_RESULT:      return LEN_RESULT;
        case MSG_APPLY:       return LEN_APPLY;
        case MSG_ABORT:       return LEN_ABORT;
        case MSG_STATUS:      return LEN_STATUS;
        case MSG_IDENTIFY:    return LEN_IDENTIFY;
        default:              return 0;
    }
}

bool isSenderToDisplay(uint8_t type) {
    return type == MSG_QUERY || type == MSG_OFFER || type == MSG_DATA ||
           type == MSG_APPLY || type == MSG_ABORT || type == MSG_IDENTIFY;
}

// Writes FE 9F <type> <version> <session>.
static void putHeader(uint8_t *out, uint8_t type, uint8_t session) {
    out[0] = HDR0; out[1] = HDR1; out[2] = type;
    out[3] = PROTO_VERSION; out[4] = session;
}

static bool checkHeader(const uint8_t *p, size_t len, uint8_t type) {
    return msgType(p, len) == type && len >= minLength(type);
}

// ---- builders ----------------------------------------------------------------------
size_t buildQuery(const QueryMsg &m, uint8_t *out, size_t cap) {
    if (!out || cap < LEN_QUERY) return 0;
    putHeader(out, MSG_QUERY, m.session);
    out[5] = m.what;
    memcpy(out + 6, m.uidFilter, 6);
    return LEN_QUERY;
}

size_t buildInfo(const InfoMsg &m, uint8_t *out, size_t cap) {
    if (!out || cap < LEN_INFO) return 0;
    putHeader(out, MSG_INFO, m.session);
    out[5] = m.protoMin;
    out[6] = m.protoMax;
    out[7] = m.boardId;
    out[8] = m.capabilities;
    memcpy(out + 9, m.uniqueId, 6);
    out[15] = m.sessionState;
    out[16] = m.otaState;
    putU16(out + 17, m.verifySecondsLeft);
    out[19] = m.runningSlot;
    out[20] = m.statusFlags;
    putU32(out + 21, m.maxFirmwareSize);
    putU32(out + 25, m.maxWebUiSize);
    out[29] = m.maxChunkSize;
    out[30] = m.maxWindowChunks;
    memcpy(out + 31, m.runningVersion, 16);
    memcpy(out + 47, m.lastInvalidVersion, 16);
    memcpy(out + 63, m.label, 16);
    memcpy(out + 79, m.runningImageId, 8);
    memcpy(out + 87, m.lastInvalidImageId, 8);
    out[95] = m.rollbackCause;
    out[96] = m.rollbackDetail;
    memcpy(out + 97, m.webUiVersion, 16);
    memcpy(out + 113, m.webUiId, 8);
    putU32(out + 121, m.webUiSize);
    putU32(out + 125, m.uptimeS);
    return LEN_INFO;
}

size_t buildOffer(const OfferMsg &m, uint8_t *out, size_t cap) {
    if (!out || cap < LEN_OFFER) return 0;
    putHeader(out, MSG_OFFER, m.session);
    out[5] = m.target;
    out[6] = m.boardId;
    out[7] = m.encoding;
    out[8] = m.flags;
    out[9] = m.chunkSize;
    putU32(out + 10, m.imageSize);
    putU32(out + 14, m.streamSize);
    memcpy(out + 18, m.sha256, 32);
    memcpy(out + 50, m.targetUid, 6);
    memcpy(out + 56, m.fwVersion, 16);
    memcpy(out + 72, m.senderName, 16);
    out[88] = m.senderKind;
    putU16(out + 89, m.estSeconds);
    putU32(out + 91, m.streamCrc);
    memcpy(out + 95, m.companionSha, 32);
    putU32(out + 127, m.companionSize);
    return LEN_OFFER;
}

size_t buildOfferReply(const OfferReplyMsg &m, uint8_t *out, size_t cap) {
    if (!out || cap < LEN_OFFER_REPLY) return 0;
    putHeader(out, MSG_OFFER_REPLY, m.session);
    out[5] = m.decision;
    out[6] = m.reason;
    putU16(out + 7, m.seconds);
    out[9] = m.windowChunks;
    return LEN_OFFER_REPLY;
}

size_t buildData(uint8_t session, uint32_t offset, const uint8_t *data, size_t n,
                 uint8_t *out, size_t cap) {
    if (!out || n < 1 || n > CHUNK_SIZE || cap < DATA_HDR_LEN + n) return 0;
    if (!data || offset > MAX_STREAM_SIZE) return 0;
    out[0] = HDR0; out[1] = HDR1; out[2] = MSG_DATA; out[3] = session;
    putU24(out + 4, offset);
    memcpy(out + DATA_HDR_LEN, data, n);
    return DATA_HDR_LEN + n;
}

size_t buildAck(const AckMsg &m, uint8_t *out, size_t cap) {
    if (!out || cap < LEN_ACK) return 0;
    putHeader(out, MSG_ACK, m.session);
    out[5] = m.state;
    putU32(out + 6, m.base);
    putU32(out + 10, m.end);
    putU64(out + 14, m.bitmap);
    putU32(out + 22, m.crc);
    putU16(out + 26, m.holdMs);
    return LEN_ACK;
}

size_t buildResult(const ResultMsg &m, uint8_t *out, size_t cap) {
    if (!out || cap < LEN_RESULT) return 0;
    putHeader(out, MSG_RESULT, m.session);
    out[5] = m.code;
    out[6] = m.phase;
    out[7] = m.detail;
    memcpy(out + 8, m.sha256, 32);
    return LEN_RESULT;
}

size_t buildApply(const ApplyMsg &m, uint8_t *out, size_t cap) {
    if (!out || cap < LEN_APPLY) return 0;
    putHeader(out, MSG_APPLY, m.session);
    out[5] = m.mode;
    return LEN_APPLY;
}

size_t buildAbort(const AbortMsg &m, uint8_t *out, size_t cap) {
    if (!out || cap < LEN_ABORT) return 0;
    putHeader(out, MSG_ABORT, m.session);
    out[5] = m.reason;
    return LEN_ABORT;
}

size_t buildStatus(const StatusMsg &m, uint8_t *out, size_t cap) {
    if (!out || cap < LEN_STATUS) return 0;
    putHeader(out, MSG_STATUS, m.session);
    out[5] = m.state;
    out[6] = m.reason;
    putU32(out + 7, m.streamAccepted);
    putU32(out + 11, m.streamSize);
    putU32(out + 15, m.imageWritten);
    putU32(out + 19, m.imageSize);
    putU16(out + 23, m.seconds);
    return LEN_STATUS;
}

size_t buildIdentify(const IdentifyMsg &m, uint8_t *out, size_t cap) {
    if (!out || cap < LEN_IDENTIFY) return 0;
    putHeader(out, MSG_IDENTIFY, m.session);
    out[5] = m.seconds;
    memcpy(out + 6, m.targetUid, 6);
    return LEN_IDENTIFY;
}

// ---- parsers ------------------------------------------------------------------------
bool parseQuery(const uint8_t *p, size_t len, QueryMsg &m) {
    if (!checkHeader(p, len, MSG_QUERY)) return false;
    m.version = p[3]; m.session = p[4]; m.what = p[5];
    memcpy(m.uidFilter, p + 6, 6);
    return true;
}

bool parseInfo(const uint8_t *p, size_t len, InfoMsg &m) {
    if (!checkHeader(p, len, MSG_INFO)) return false;
    m.version = p[3]; m.session = p[4];
    m.protoMin = p[5]; m.protoMax = p[6]; m.boardId = p[7]; m.capabilities = p[8];
    memcpy(m.uniqueId, p + 9, 6);
    m.sessionState = p[15]; m.otaState = p[16];
    m.verifySecondsLeft = getU16(p + 17);
    m.runningSlot = p[19]; m.statusFlags = p[20];
    m.maxFirmwareSize = getU32(p + 21);
    m.maxWebUiSize = getU32(p + 25);
    m.maxChunkSize = p[29]; m.maxWindowChunks = p[30];
    memcpy(m.runningVersion, p + 31, 16);
    memcpy(m.lastInvalidVersion, p + 47, 16);
    memcpy(m.label, p + 63, 16);
    memcpy(m.runningImageId, p + 79, 8);
    memcpy(m.lastInvalidImageId, p + 87, 8);
    m.rollbackCause = p[95]; m.rollbackDetail = p[96];
    memcpy(m.webUiVersion, p + 97, 16);
    memcpy(m.webUiId, p + 113, 8);
    m.webUiSize = getU32(p + 121);
    m.uptimeS = getU32(p + 125);
    return true;
}

bool parseOffer(const uint8_t *p, size_t len, OfferMsg &m) {
    if (!checkHeader(p, len, MSG_OFFER)) return false;
    m.version = p[3]; m.session = p[4];
    m.target = p[5]; m.boardId = p[6]; m.encoding = p[7]; m.flags = p[8];
    m.chunkSize = p[9];
    m.imageSize = getU32(p + 10);
    m.streamSize = getU32(p + 14);
    memcpy(m.sha256, p + 18, 32);
    memcpy(m.targetUid, p + 50, 6);
    memcpy(m.fwVersion, p + 56, 16);
    memcpy(m.senderName, p + 72, 16);
    m.senderKind = p[88];
    m.estSeconds = getU16(p + 89);
    m.streamCrc = getU32(p + 91);
    memcpy(m.companionSha, p + 95, 32);
    m.companionSize = getU32(p + 127);
    return true;
}

bool parseOfferReply(const uint8_t *p, size_t len, OfferReplyMsg &m) {
    if (!checkHeader(p, len, MSG_OFFER_REPLY)) return false;
    m.version = p[3]; m.session = p[4];
    m.decision = p[5]; m.reason = p[6];
    m.seconds = getU16(p + 7);
    m.windowChunks = p[9];
    return true;
}

bool parseData(const uint8_t *p, size_t len, DataMsg &m) {
    if (!checkHeader(p, len, MSG_DATA)) return false;
    // A DATA message is at most one chunk; anything longer is not v1 DATA
    // (DATA carries no version byte, so it has no room for appended fields).
    if (len > LEN_DATA_FULL) return false;
    m.session = p[3];
    m.offset = getU24(p + 4);
    m.data = p + DATA_HDR_LEN;
    m.len = (uint16_t)(len - DATA_HDR_LEN);
    return true;
}

bool parseAck(const uint8_t *p, size_t len, AckMsg &m) {
    if (!checkHeader(p, len, MSG_ACK)) return false;
    m.version = p[3]; m.session = p[4]; m.state = p[5];
    m.base = getU32(p + 6);
    m.end = getU32(p + 10);
    m.bitmap = getU64(p + 14);
    m.crc = getU32(p + 22);
    m.holdMs = getU16(p + 26);
    return true;
}

bool parseResult(const uint8_t *p, size_t len, ResultMsg &m) {
    if (!checkHeader(p, len, MSG_RESULT)) return false;
    m.version = p[3]; m.session = p[4];
    m.code = p[5]; m.phase = p[6]; m.detail = p[7];
    memcpy(m.sha256, p + 8, 32);
    return true;
}

bool parseApply(const uint8_t *p, size_t len, ApplyMsg &m) {
    if (!checkHeader(p, len, MSG_APPLY)) return false;
    m.version = p[3]; m.session = p[4]; m.mode = p[5];
    return true;
}

bool parseAbort(const uint8_t *p, size_t len, AbortMsg &m) {
    if (!checkHeader(p, len, MSG_ABORT)) return false;
    m.version = p[3]; m.session = p[4]; m.reason = p[5];
    return true;
}

bool parseStatus(const uint8_t *p, size_t len, StatusMsg &m) {
    if (!checkHeader(p, len, MSG_STATUS)) return false;
    m.version = p[3]; m.session = p[4]; m.state = p[5]; m.reason = p[6];
    m.streamAccepted = getU32(p + 7);
    m.streamSize = getU32(p + 11);
    m.imageWritten = getU32(p + 15);
    m.imageSize = getU32(p + 19);
    m.seconds = getU16(p + 23);
    return true;
}

bool parseIdentify(const uint8_t *p, size_t len, IdentifyMsg &m) {
    if (!checkHeader(p, len, MSG_IDENTIFY)) return false;
    m.version = p[3]; m.session = p[4]; m.seconds = p[5];
    memcpy(m.targetUid, p + 6, 6);
    return true;
}

// ---- CRC-32 -------------------------------------------------------------------------
// Nibble table for the reflected polynomial 0xEDB88320: 64 bytes of rodata
// instead of 1 KB. The device may hand the ROM routine in instead (Crc32Fn).
uint32_t crc32Update(uint32_t prev, const uint8_t *p, size_t n) {
    static const uint32_t T[16] = {
        0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
        0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
        0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
        0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL };
    uint32_t c = ~prev;
    while (n--) {
        c ^= *p++;
        c = (c >> 4) ^ T[c & 0x0F];
        c = (c >> 4) ^ T[c & 0x0F];
    }
    return ~c;
}

// ---- field helpers -------------------------------------------------------------------
bool isValidVersionField(const uint8_t f[16]) {
    size_t n = 0;
    while (n < 16 && f[n] != 0) {
        if (f[n] < 0x20 || f[n] > 0x7E) return false;
        n++;
    }
    if (n < 1 || n > 15) return false;          // 16 characters leave no NUL
    for (size_t i = n; i < 16; i++)
        if (f[i] != 0) return false;            // NUL-padded, nothing after it
    return true;
}

bool putVersionField(uint8_t out[16], const char *s) {
    memset(out, 0, 16);
    if (!s) return false;
    bool ok = true;
    size_t i = 0;
    for (; s[i] && i < 15; i++) {
        const uint8_t c = (uint8_t)s[i];
        if (c < 0x20 || c > 0x7E) ok = false;
        out[i] = c;
    }
    if (s[i]) ok = false;                       // cut
    return ok && i > 0;
}

// Length of the UTF-8 sequence that starts with `lead`, 1 for anything that
// cannot start one (a stray continuation byte is copied alone).
static size_t utf8SeqLen(uint8_t lead) {
    if (lead < 0x80) return 1;
    if (lead >= 0xC2 && lead <= 0xDF) return 2;
    if (lead >= 0xE0 && lead <= 0xEF) return 3;
    if (lead >= 0xF0 && lead <= 0xF4) return 4;
    return 1;
}

void putUtf8Field(uint8_t out[16], const char *s) {
    memset(out, 0, 16);
    if (!s) return;
    size_t i = 0;
    while (s[i]) {
        const size_t n = utf8SeqLen((uint8_t)s[i]);
        if (i + n > 16) break;                  // never cut a character
        size_t k = 1;
        while (k < n && s[i + k]) k++;
        if (k < n) break;                       // truncated sequence at the end
        memcpy(out + i, s + i, n);
        i += n;
    }
}

bool isAllZero(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) if (p[i]) return false;
    return true;
}

bool isAllFF(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) if (p[i] != 0xFF) return false;
    return true;
}

// The non-ASCII glyphs the display fonts cover (3.0).
static bool drawableCodePoint(uint32_t cp) {
    if (cp >= 0x20 && cp <= 0x7E) return true;
    switch (cp) {
        case 0x00B0: case 0x2022: case 0x00B1: case 0x00C4: case 0x00D6:
        case 0x00D7: case 0x00DC: case 0x00DF: case 0x00E4: case 0x00F6:
        case 0x00FC: case 0x2013: case 0x201C: case 0x201D: case 0x201E:
            return true;
        default:
            return false;
    }
}

size_t sanitizeText(const uint8_t *in, size_t inLen, char *out, size_t outCap) {
    if (!out || outCap == 0) return 0;
    size_t o = 0;
    size_t i = 0;
    while (in && i < inLen && in[i] != 0) {
        const uint8_t lead = in[i];
        size_t n = utf8SeqLen(lead);
        uint32_t cp = 0xFFFFFFFFUL;              // invalid until proven otherwise
        if (n == 1) {
            if (lead < 0x80) cp = lead;
        } else if (i + n <= inLen) {
            uint32_t v = (n == 2) ? (lead & 0x1Fu) : (n == 3) ? (lead & 0x0Fu) : (lead & 0x07u);
            bool good = true;
            for (size_t k = 1; k < n; k++) {
                const uint8_t c = in[i + k];
                if ((c & 0xC0) != 0x80) { good = false; break; }
                v = (v << 6) | (c & 0x3Fu);
            }
            // Overlong forms and surrogates are broken UTF-8, not characters.
            if (good && n == 3 && (v < 0x800 || (v >= 0xD800 && v <= 0xDFFF))) good = false;
            if (good && n == 4 && (v < 0x10000 || v > 0x10FFFF)) good = false;
            if (good) cp = v;
        }
        if (cp == 0xFFFFFFFFUL) n = 1;           // skip one byte of garbage
        uint8_t enc[3];
        size_t encLen;
        if (drawableCodePoint(cp)) {
            if (cp < 0x80)       { enc[0] = (uint8_t)cp; encLen = 1; }
            else if (cp < 0x800) { enc[0] = (uint8_t)(0xC0 | (cp >> 6));
                                   enc[1] = (uint8_t)(0x80 | (cp & 0x3F)); encLen = 2; }
            else                 { enc[0] = (uint8_t)(0xE0 | (cp >> 12));
                                   enc[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                                   enc[2] = (uint8_t)(0x80 | (cp & 0x3F)); encLen = 3; }
        } else {
            enc[0] = '?'; encLen = 1;
        }
        if (o + encLen + 1 > outCap) break;
        memcpy(out + o, enc, encLen);
        o += encLen;
        i += n;
    }
    out[o] = 0;
    return o;
}

// One component of a dotted version: a number, then any non-dot suffix.
static const char *versionComponent(const char *s, uint32_t &num, const char *&suf,
                                    size_t &sufLen, bool &hasDigits) {
    num = 0; hasDigits = false;
    while (*s >= '0' && *s <= '9') {
        hasDigits = true;
        if (num < 100000000UL) num = num * 10 + (uint32_t)(*s - '0');
        s++;
    }
    suf = s; sufLen = 0;
    while (*s && *s != '.') { s++; sufLen++; }
    if (*s == '.') s++;
    return s;
}

int compareVersions(const char *a, const char *b) {
    if (!a || !b || !(*a >= '0' && *a <= '9') || !(*b >= '0' && *b <= '9')) return 2;
    while (*a || *b) {
        uint32_t na, nb; const char *sa, *sb; size_t la, lb; bool da, db;
        a = versionComponent(a, na, sa, la, da);
        b = versionComponent(b, nb, sb, lb, db);
        if (na != nb) return na < nb ? -1 : 1;
        // Same number: no suffix ("1.3.0") ranks above one ("1.3.0-rc1").
        if (la != lb || (la && memcmp(sa, sb, la) != 0)) {
            if (la == 0) return 1;
            if (lb == 0) return -1;
            const size_t m = la < lb ? la : lb;
            const int c = memcmp(sa, sb, m);
            if (c != 0) return c < 0 ? -1 : 1;
            return la < lb ? -1 : 1;
        }
    }
    return 0;
}

// ---- firmware descriptor ----------------------------------------------------------
bool isDescriptorMagic(const uint8_t *p) {
    static const uint8_t M[8] = { 'N', 'P', 'F', 'W', 'D', 'E', 'S', 'C' };
    return p && memcmp(p, M, 8) == 0;
}

bool parseDescriptor(const uint8_t *p, size_t len, FwDesc &d) {
    if (!p || len < FWDESC_SIZE || !isDescriptorMagic(p)) return false;
    d.descVersion = p[0x08];
    d.boardId     = p[0x09];
    d.descSize    = getU16(p + 0x0A);
    d.panelW      = getU16(p + 0x0C);
    d.panelH      = getU16(p + 0x0E);
    d.n2kProto    = p[0x10];
    d.flags       = p[0x11];
    memcpy(d.fwVersion, p + 0x14, 16);
    memcpy(d.boardName, p + 0x24, 28);
    return true;
}

size_t buildDescriptor(const FwDesc &d, uint8_t *out, size_t cap) {
    if (!out || cap < FWDESC_SIZE) return 0;
    static const uint8_t M[8] = { 'N', 'P', 'F', 'W', 'D', 'E', 'S', 'C' };
    memset(out, 0, FWDESC_SIZE);
    memcpy(out, M, 8);
    out[0x08] = d.descVersion;
    out[0x09] = d.boardId;
    putU16(out + 0x0A, d.descSize);
    putU16(out + 0x0C, d.panelW);
    putU16(out + 0x0E, d.panelH);
    out[0x10] = d.n2kProto;
    out[0x11] = d.flags;
    memcpy(out + 0x14, d.fwVersion, 16);
    memcpy(out + 0x24, d.boardName, 28);
    return FWDESC_SIZE;
}

uint8_t checkImageIdentity(const uint8_t *head, size_t headLen,
                           const uint8_t *desc, size_t descLen,
                           const DescExpect &expect) {
    if (!head || headLen < APP_DESC_OFFSET + 4) return RSN_DESCRIPTOR_INVALID;
    if (head[0] != IMAGE_MAGIC) return RSN_IMAGE_INVALID;
    if (getU32(head + APP_DESC_OFFSET) != APP_DESC_MAGIC) return RSN_DESCRIPTOR_INVALID;
    FwDesc d;
    if (!parseDescriptor(desc, descLen, d)) return RSN_DESCRIPTOR_INVALID;  // legacy
    if (d.descVersion < 1 || d.descSize < FWDESC_SIZE) return RSN_DESCRIPTOR_INVALID;
    // Board first: an image for another board is WRONG_BOARD even when the
    // rest of its descriptor would not fit either.
    if (d.boardId != expect.boardId) return RSN_WRONG_BOARD;
    if (d.panelW != expect.panelW || d.panelH != expect.panelH) return RSN_DESCRIPTOR_INVALID;
    if (!isValidVersionField(d.fwVersion)) return RSN_DESCRIPTOR_INVALID;
    if (expect.version16 && memcmp(d.fwVersion, expect.version16, 16) != 0)
        return RSN_DESCRIPTOR_INVALID;
    if (!(d.flags & FWDESC_F_OTAGUARD)) return RSN_DESCRIPTOR_INVALID;
    return RSN_OK;
}

}  // namespace n2kupd
