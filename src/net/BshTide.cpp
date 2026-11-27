#include "BshTide.h"
#include <Arduino.h>
#include <esp_idf_version.h>
#if ESP_IDF_VERSION_MAJOR >= 5
#include "freertos/idf_additions.h"   // xTaskCreatePinnedToCoreWithCaps
#endif
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include <esp_sntp.h>           // esp_sntp_* - in IDF 4.4.7 and 5.5 alike
#include <atomic>
#include <time.h>
#include <math.h>
#include "../nmea/DataModel.h"
#include "../config/Config.h"   // cfg.tideBshFetch gates the fetch, not the clock
#include "../SunCalc.h"
#include "../Version.h"         // FW_VERSION, for the User-Agent

// ── JSON memory ──────────────────────────────────────────────────────────────
// The BSH body is huge: 433 KB for the query below (measured, limit=6), nearly
// all of it the per-station 'curve' that the filter skips. What the filter
// keeps is bounded twice: by WHERE it may live and by HOW MUCH there is.
//
// Where. Every block ArduinoJson allocates here is <= 1 KB and therefore
// below SPIRAM_MALLOC_ALWAYSINTERNAL, so the DEFAULT allocator takes ALL of it
// from internal DRAM - while the TLS session is open and while the WiFi RX and
// lwIP buffers, which platformio.ini deliberately keeps internal, need that
// same DRAM. That is what first killed this fetch: the RX path starved
// mid-body, the stream ran dry - which on core 3.x ended the parse on the
// spot, not after the 15 s the two setTimeout() calls suggest (see
// BshBodyReader below) - ArduinoJson reported IncompleteInput on a request
// that had already returned 200, and whichever task lost the next allocation
// took the device down with it. Same failure and same cure as the config JSON
// (Config.cpp). No DRAM fallback on purpose - it would reinstate exactly the
// spike this removes; a failed PSRAM block surfaces as NoMemory.
// IDF 5 only: the 4" stays on its proven DRAM path (its IDF 4.4 carries the
// documented OPI-PSRAM coherency bug), through malloc() exactly as
// ArduinoJson's default allocator would.
//
// How much. PSRAM is no bottomless pit on the 7B/5B either: the canvas arena
// and the frame buffer are carved out at boot, the heap keeps 57.9 KB free
// (the PSRAM= heartbeat field, byte-stable from boot on, screen visits or
// not - canvases come from the arena), and the TLS session with the HTTP
// client holds ~45 KB of that during a fetch (mbedTLS EXTERNAL_MEM_ALLOC: two
// 16 KB record buffers and their contexts) - heartbeats taken mid-fetch read
// 8.4 KB free 2-3 s in, with ~4 KB of tree built. ~12 KB left, then, and one
// tree for the whole answer - six stations with all their ~23 events, 14.2 KB
// in 152 blocks, 17.3 KB of contiguous heap with the block overhead (PC
// harness, same ArduinoJson, 32-bit layout, on a saved body) - did not fit:
// NoMemory 96-98 % into the body on every try (seen 2026-10-30, once
// BshBodyReader had stopped IncompleteInput from hiding it). Hence the body
// walker below, one station's tree at a time: 3.4 KB in 38 blocks, 4.1 KB of
// heap. The 4" profits the same way, with its tree and its TLS in DRAM.
//
// TLS. On the 7B/5B mbedTLS keeps its two 16 KB record buffers in PSRAM
// (platformio.ini, MBEDTLS_EXTERNAL_MEM_ALLOC). The S3's DMA AES could not
// decrypt into them directly - a record's plaintext never starts on a cache
// line - and took every record through a 1.6 KB bounce buffer of internal
// DMA-capable RAM, allocated per record against the WiFi buffers. When none
// was free: "esp-aes: Failed to allocate memory", the TLS read failed, and
// the body ended as IncompleteInput - on most long fetches (seen until
// 12.11.2026). AES has run in software there since (MBEDTLS_HARDWARE_AES=n,
// platformio.ini): it decrypts in place and allocates nothing. Mind the
// "largest block" in the error line below: MALLOC_CAP_INTERNAL includes the
// 8 KB RTC FAST heap, which no DMA reaches, and its ~7.5 KB block was all
// that line showed during those failures. The bench line (-DBSH_BENCH_*)
// gives the DMA-capable figure as well.
//
// The allocator counts what the trees hold (heap_caps_get_allocated_size, on
// both cores), so the log line after a fetch shows the peak.
#if ESP_IDF_VERSION_MAJOR >= 5
#define BSH_JSON_MALLOC(n)     heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define BSH_JSON_REALLOC(p, n) heap_caps_realloc((p), (n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define BSH_JSON_FREE(p)       heap_caps_free(p)
#else
#define BSH_JSON_MALLOC(n)     malloc(n)
#define BSH_JSON_REALLOC(p, n) realloc((p), (n))
#define BSH_JSON_FREE(p)       free(p)
#endif
struct BshJsonAllocator final : ArduinoJson::Allocator {
    void *allocate(size_t n) override {
        void *p = BSH_JSON_MALLOC(n);
        if (p) grow(heap_caps_get_allocated_size(p));
        return p;
    }
    void deallocate(void *p) override {
        if (p) live -= heap_caps_get_allocated_size(p);
        BSH_JSON_FREE(p);
    }
    void *reallocate(void *p, size_t n) override {
        const size_t before = p ? heap_caps_get_allocated_size(p) : 0;
        void *q = BSH_JSON_REALLOC(p, n);
        if (q) { live -= before; grow(heap_caps_get_allocated_size(q)); }
        return q;                                         // failed: p untouched
    }
    size_t live = 0, peak = 0;                            // bytes, BSH task only
private:
    void grow(size_t n) { live += n; if (live > peak) peak = live; }
};
static BshJsonAllocator s_bshJson;
#define BSH_JSON_DOC(name) JsonDocument name(&s_bshJson)

// ── Configuration ────────────────────────────────────────────────────────────
static const char *BSH_URL_BASE =
    "https://gdi.bsh.de/ldproxy/rest/services/WaterLevelForecast"
    "/collections/waterlevelforecastdata/items";
// Who is asking: the BSH sees this project and where to find it, instead of
// HTTPClient's anonymous default "ESP32HTTPClient".
static const char *BSH_USER_AGENT =
    "NauticPinnace/" FW_VERSION " (+https://github.com/Ranman86/NauticPinnace)";
// Europe/Berlin with automatic DST (CET/CEST).
static const char *TZ_BERLIN = "CET-1CEST,M3.5.0,M10.5.0/3";
// Fallback position (Cuxhaven) when no GPS fix is available yet.
static constexpr float FALLBACK_LAT = 53.870f, FALLBACK_LON = 8.720f;

// Every 3 h: the BSH issues its forecast several times a day, and each answer
// is the whole ~433 KB - half-hourly fetches mostly fetched the same data
// again, about 20 MB a day near the coast.
static constexpr uint32_t FETCH_PERIOD_MS = 3UL * 60UL * 60UL * 1000UL;  // 3 h
static constexpr int      RESYNC_EVERY    = 8;                            // ~24 h
// No WiFi at a scheduled pass: look again after a minute instead of a whole
// period. That only reads WiFi.status() - no request goes out - and spares a
// late or background join (fallback hotspot first, the network found a minute
// later) three hours without clock sync and forecast.
static constexpr uint32_t WIFI_RECHECK_MS = 60UL * 1000UL;         // 1 min
// A FAILED fetch - no connection, TLS, an HTTP error, any JSON error (a cut
// body, no PSRAM for the tree) - is asked again after 2 min and, failing
// again, after another 10; after that the regular slots carry on alone until
// something comes back. At most two extra requests per outage, so a long one
// still costs one per period. An answer without usable data (no gauge, nothing
// ahead) is never retried: it reads the same two minutes later. Retries never
// bring the next slot forward (only the 2-min floor below can push it back a
// little) and never count as a pass, so the ~24 h SNTP resync keeps its rhythm.
static constexpr uint32_t RETRY_MS[]      = { 2UL * 60UL * 1000UL,     // 2 min
                                              10UL * 60UL * 1000UL };  // 10 min
static constexpr int      RETRY_COUNT     = (int)(sizeof(RETRY_MS) / sizeof(RETRY_MS[0]));
// The body ends after this long without a single byte (BshBodyReader).
static constexpr uint32_t BODY_IDLE_MS    = 15000;

// ── Bench builds ─────────────────────────────────────────────────────────────
// Two test flags for measuring this fetch at the desk. Neither may ever reach
// a release: each prints a fixed "TEST BUILD" line at start (bshTideBegin()),
// and tools/release_checks.py (check testbuild) refuses an image carrying it.
// Without them nothing below exists and the fetch is the one described above.
//   -DBSH_BENCH_POS          every request asks for 53.870 N 8.720 E
//                            (Cuxhaven: six gauges, the ~430 KB answer),
//                            whatever position the bus sends, and unrounded
//                            (fetchBsh()). A bench bus far from any BSH gauge
//                            gets an 853 B answer, and the long transfer
//                            this fetch is about never runs.
//   -DBSH_BENCH_EVERY_S=<s>  the next fetch <s> seconds (30 at the least)
//                            after the last one ended, whatever it came back
//                            with: no retries, no 3-h slots, so n passes
//                            give n fetches of the same kind. The first still
//                            waits for the boot to settle (bshTask()).
// Either flag adds one line at the end of every fetch, on every way out of
// fetchBsh() (bshBenchLine()).
#if defined(BSH_BENCH_POS) || defined(BSH_BENCH_EVERY_S)
#define BSH_BENCH 1
#else
#define BSH_BENCH 0
#endif
#if defined(BSH_BENCH_POS)
static constexpr float BENCH_LAT = 53.870f, BENCH_LON = 8.720f;   // Cuxhaven
#endif
#if defined(BSH_BENCH_EVERY_S)
static_assert((BSH_BENCH_EVERY_S) >= 30,
              "BSH_BENCH_EVERY_S: 30 s at the least - every fetch is a 430 KB "
              "request to a public service");
static constexpr uint32_t BENCH_PERIOD_MS = (uint32_t)(BSH_BENCH_EVERY_S) * 1000UL;
#endif
#if BSH_BENCH
// The heap at the end of a fetch, taken while the TLS session still stands
// (before http.end()), where the error line in fetchBsh() takes its own.
// MALLOC_CAP_INTERNAL includes the 8 KB RTC FAST heap, which no DMA reaches;
// the dma_ fields count only internal RAM that DMA can use - what the WiFi
// buffers and the AES driver's bounce buffer are allocated from.
struct BshBenchHeap {
    unsigned intFree = 0, intBlk = 0, dmaFree = 0, dmaBlk = 0, psFree = 0, psBlk = 0;
    bool     taken   = false;
    void take() {
        intFree = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        intBlk  = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        dmaFree = (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        dmaBlk  = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        psFree  = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        psBlk   = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        taken   = true;
    }
};
// One line per fetch, in a fixed key=value form for the bench scripts:
//   [BSH] bench n=<fetch since boot> up=<s>s result=<published|nodata|failed>
//         why="<...>" stations=<features> body=<B> ms=<GET to body end>
//         int_free=<B> int_blk=<B> dma_free=<B> dma_blk=<B>
//         psram_free=<B> psram_blk=<B>
// 'why' is the station published, or what ended the fetch (HTTP -5, JSON
// IncompleteInput, no stations in bbox, ...). A fetch that ended before the
// transfer passes no heap and gets the heap as it is then.
static void bshBenchLine(const char *result, const char *why, int stations,
                         uint32_t bodyBytes, uint32_t ms, BshBenchHeap h) {
    static uint32_t n = 0;
    if (!h.taken) h.take();
    Serial.printf("[BSH] bench n=%lu up=%lus result=%s why=\"%s\" stations=%d body=%lu ms=%lu "
                  "int_free=%u int_blk=%u dma_free=%u dma_blk=%u psram_free=%u psram_blk=%u\n",
                  (unsigned long)++n, (unsigned long)(millis() / 1000), result, why, stations,
                  (unsigned long)bodyBytes, (unsigned long)ms, h.intFree, h.intBlk,
                  h.dmaFree, h.dmaBlk, h.psFree, h.psBlk);
}
#endif

// ── Time helpers ─────────────────────────────────────────────────────────────
// Parse "2026-06-11 16:31:00+02:00" -> Unix seconds (UTC). 0 on failure.
static uint32_t parseBshTime(const char *s) {
    if (!s) return 0;
    int Y, Mo, D, h, m, sec, oh = 0, om = 0; char sign = '+';
    int got = sscanf(s, "%d-%d-%d %d:%d:%d%c%d:%d",
                     &Y, &Mo, &D, &h, &m, &sec, &sign, &oh, &om);
    if (got < 6) return 0;
    long days     = scDaysFromCivil(Y, (unsigned)Mo, (unsigned)D);
    long localSec = days * 86400L + h * 3600L + m * 60L + sec;
    long offSec   = (long)(oh * 3600 + om * 60) * (sign == '-' ? -1 : 1);
    return (uint32_t)(localSec - offSec);
}

// Local (Berlin, DST-aware) minus UTC at `t` in minutes, from the TZ rule that
// sntpStart() installed (below) - derived from the two clock readings.
static int ruleOffsetMin(time_t t) {
    struct tm lt, gt; localtime_r(&t, &lt); gmtime_r(&t, &gt);
    int diff = (lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec)
             - (gt.tm_hour * 3600 + gt.tm_min * 60 + gt.tm_sec);
    if (diff < -43200) diff += 86400;
    if (diff >  43200) diff -= 86400;
    return diff / 60;
}

// ── Clock sync (SNTP) ────────────────────────────────────────────────────────
// Which time server: the one the router names in its DHCP lease (option 42),
// if it names one, else the public pool. A router's server that has not
// answered with a set clock NTP_ROUTER_GRACE_MS after SNTP was started on it
// is dropped for the pool until the next boot.
//
// What this relies on in lwIP (sntp.c, dhcp.c - the same in IDF 4.4.7 and 5.5):
//  - A server from DHCP is taken only while "server mode DHCP" is on, and only
//    when a DHCP ACK comes in: dhcp_handle_ack() passes option 42 to
//    dhcp_set_ntp_servers(), which writes the address into slot 0 and clears
//    slots 1..2, names included. A lease without option 42 changes nothing.
//    Hence bshSntpPreferDhcp() before the first join (setup()).
//  - configTzTime() writes NAMES into slots 0..2 - it would throw the router's
//    server away. So with a router's server SNTP is (re)started without it:
//    configTzTime()'s own stop + init, through esp_sntp_*, which hand each
//    step to the tcpip thread (tcpip_callback(), queued in order). That keeps
//    them inside lwIP's core lock on IDF 5 (core locking and its thread-safety
//    asserts are on there), as configTzTime() does by taking the lock itself
//    (esp32-hal-time.c); IDF 4.4 has no core locking.
//  - Falling back, the mode goes off first: the next lease renewal would put
//    the silent router back over the pool names otherwise.
//  - lwIP keeps asking by itself, every CONFIG_LWIP_SNTP_UPDATE_DELAY (3 h)
//    after an answer; without one, again after 15 s and, depending on the
//    number of servers, backing off to a few minutes.
static constexpr uint32_t NTP_ROUTER_GRACE_MS = 10UL * 60UL * 1000UL;  // 10 min
static constexpr uint32_t NTP_CHECK_MS        = 10UL * 1000UL;         // 10 s
enum class NtpSrc : uint8_t { None, Router, Pool };
static NtpSrc   s_ntpSrc       = NtpSrc::None;
static bool     s_ntpDhcpMode  = false;   // server mode DHCP is on (bshSntpPreferDhcp())
static uint32_t s_ntpStartMs   = 0;       // SNTP started on the router's server ...
static uint32_t s_ntpRepliesAt = 0;       // ... with this many answers counted then
static std::atomic<uint32_t> s_ntpReplies{0};   // answers with a set clock (> 2023-11)

// lwIP calls this in the tcpip thread after an answer has set the clock. lwIP
// takes any answer but a Kiss-of-Death, also one from a server whose own clock
// is unset (leap 3, stratum 16): only an answer with a plausible time counts,
// so such a server goes to the pool after NTP_ROUTER_GRACE_MS like a silent one.
static void onSntpSync(struct timeval *tv) {
    if (tv && tv->tv_sec > 1700000000) s_ntpReplies.fetch_add(1);   // as publishSntpTime()
}

void bshSntpPreferDhcp() {
#if LWIP_DHCP_GET_NTP_SRV
    esp_netif_init();                     // as configTzTime() does; no-op once WiFi.mode() ran
    esp_sntp_servermode_dhcp(true);
    s_ntpDhcpMode = true;
#endif
}

// Started on the router's server and no answer with a set clock since.
static bool sntpRouterSilent() {
    return s_ntpSrc == NtpSrc::Router && s_ntpReplies.load() == s_ntpRepliesAt;
}

// The pool: Arduino's configTzTime() stops SNTP, names the servers, starts it.
static void sntpUsePool(const char *why) {
#if LWIP_DHCP_GET_NTP_SRV
    if (s_ntpDhcpMode) esp_sntp_servermode_dhcp(false);
#endif
    s_ntpDhcpMode = false;
    // TODO: replace the two pool names with the project's own NTP Pool vendor
    // zone once ntppool.org has granted one - the pool asks that a device's
    // default setup use a vendor zone, not pool.ntp.org itself.
    configTzTime(TZ_BERLIN, "pool.ntp.org", "de.pool.ntp.org");
    if (s_ntpSrc != NtpSrc::Pool) Serial.printf("[BSH] SNTP: pool.ntp.org (%s)\n", why);
    s_ntpSrc = NtpSrc::Pool;
}

// (Re)starts SNTP: at the first pass with WiFi, on every pass until the clock
// is set, and at the ~24 h resync.
static void sntpStart() {
    sntp_set_time_sync_notification_cb(onSntpSync);
    if (s_ntpDhcpMode) {
        const ip_addr_t srv = *esp_sntp_getserver(0);   // what the lease left there
        if (!ip_addr_isany_val(srv)) {
            // No setoperatingmode(): poll is lwIP's default and nothing here
            // sets another, and lwIP asserts if it runs on a client that is
            // still up. stop() and init() are no-ops when there is nothing to do.
            esp_sntp_stop();
            esp_sntp_init();
            setenv("TZ", TZ_BERLIN, 1);
            tzset();
            // A restart while the server is still silent keeps its deadline.
            if (!sntpRouterSilent()) {
                s_ntpStartMs   = millis();
                s_ntpRepliesAt = s_ntpReplies.load();
            }
            if (s_ntpSrc != NtpSrc::Router) {
                char ip[48];
                Serial.printf("[BSH] SNTP: time server %s from the DHCP lease\n",
                              ipaddr_ntoa_r(&srv, ip, sizeof(ip)));
            }
            s_ntpSrc = NtpSrc::Router;
            return;
        }
    }
    sntpUsePool("no time server from DHCP");
}

// Publishes the system clock to the DataModel once SNTP has set it. Returns
// the current Unix time, 0 while the clock is unset.
static uint32_t publishSntpTime() {
    const time_t now = time(nullptr);
    if (now <= 1700000000) return 0;          // > 2023-11 → clock is set
    int offMin = ruleOffsetMin(now);
    {
        auto lk = data.lock();
        data.sysDays           = (uint16_t)(now / 86400);
        data.sysSecOfDay       = (double)(now % 86400);
        data.localOffsetMin    = (int16_t)offMin;
        data.localOffsetIsRule = true;
        data.lastTimeUpdate    = millis();
        data.timeValid         = true;
        data.timeIsReal        = true;
    }
    Serial.printf("[BSH] SNTP ok: unix=%ld  offset=%+dmin\n", (long)now, offMin);
    return (uint32_t)now;
}

// (Re)starts SNTP and waits up to ~12 s for the clock. Returns the current
// Unix time on success, 0 on failure - lwIP keeps asking, and bshSleep()
// publishes a later answer.
static uint32_t syncTimeFromSntp() {
    sntpStart();
    for (int i = 0; i < 24 && time(nullptr) <= 1700000000; i++)   // wait up to ~12 s
        vTaskDelay(pdMS_TO_TICKS(500));
    return publishSntpTime();
}

// The pause between two passes. While SNTP runs and the clock is still unset,
// or the router's server has not answered with a set clock yet, it is cut into NTP_CHECK_MS
// steps, so that a late answer reaches the clock page and the pool takes over
// in time - not only at the next pass, up to 3 h away. A step reads a clock
// and a counter; nothing goes out.
static void bshSleep(uint32_t waitMs, bool &synced) {
    const uint32_t t0 = millis();
    for (;;) {
        const uint32_t gone = millis() - t0;
        if (gone >= waitMs) return;
        uint32_t step = waitMs - gone;
        const bool watch = s_ntpSrc != NtpSrc::None && (!synced || sntpRouterSilent());
        if (watch && step > NTP_CHECK_MS) step = NTP_CHECK_MS;
        // Not pdMS_TO_TICKS(): it multiplies in 32 bits and wraps past ~71 min
        // at 1000 Hz on both IDFs - the 3 h period came out as 37 min.
        vTaskDelay((TickType_t)((uint64_t)step * configTICK_RATE_HZ / 1000U));
        if (!watch || WiFi.status() != WL_CONNECTED) continue;
        if (sntpRouterSilent() && millis() - s_ntpStartMs >= NTP_ROUTER_GRACE_MS)
            sntpUsePool("the time server from DHCP gave no set clock within 10 min");
        if (!synced && publishSntpTime()) synced = true;
    }
}

// publishSntpTime() takes the offset at the sync, and the next sync is ~24 h
// away: on its own it would show a DST switch on the clock up to a day late.
// While it is still the rule's - no bus offset (PGN 129033) has replaced it
// since the sync or since clearValues() handed it back to the rule - it is
// read again from the rule every second: the Clock screen shows seconds,
// and the read is cheap (one localtime_r + gmtime_r). The system clock keeps
// running between syncs, so time() is the instant to ask for.
void bshLocalOffsetTick() {
    static uint32_t lastMs = 0;
    const uint32_t nowMs = millis();
    if (lastMs != 0 && nowMs - lastMs < 1000) return;
    lastMs = nowMs;
    int16_t cur;
    {
        auto lk = data.lock();
        if (!data.localOffsetIsRule) return;
        cur = data.localOffsetMin;
    }
    const int off = ruleOffsetMin(time(nullptr));
    if (off == cur) return;
    {
        auto lk = data.lock();
        if (!data.localOffsetIsRule) return;   // a bus offset came in meanwhile
        data.localOffsetMin = (int16_t)off;
    }
    Serial.printf("[BSH] local offset %+d -> %+d min (Europe/Berlin DST rule)\n", cur, off);
}

// Transliterate UTF-8 German text to ASCII (the UI fonts have no umlauts).
static void translit(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 1 < cap; ) {
        unsigned char c = (unsigned char)in[i];
        if (c == 0xC3 && in[i + 1]) {                       // 2-byte Latin-1
            const char *r = nullptr;
            switch ((unsigned char)in[i + 1]) {
                case 0xA4: r = "ae"; break; case 0x84: r = "Ae"; break;
                case 0xB6: r = "oe"; break; case 0x96: r = "Oe"; break;
                case 0xBC: r = "ue"; break; case 0x9C: r = "Ue"; break;
                case 0x9F: r = "ss"; break;
            }
            if (r) { while (*r && o + 1 < cap) out[o++] = *r++; }
            i += 2; continue;
        }
        if (c < 0x80) { out[o++] = (char)c; i++; continue; } // plain ASCII
        // Skip an unmapped multi-byte sequence, but never step OVER the
        // terminator: on a truncated UTF-8 tail (or a lone trailing 0xC3) the
        // blind "i += 2/3/4" landed past the NUL and the loop then kept reading
        // off the end of the string.
        size_t skip = (c < 0xE0) ? 2 : (c < 0xF0 ? 3 : 4);
        while (skip-- && in[i]) i++;
    }
    out[o] = 0;
}

// ── Body reader ──────────────────────────────────────────────────────────────
// What ArduinoJson pulls the 433 KB body through. Handing it http.getStream()
// looked like a 15 s timeout and was none. ArduinoJson reads every byte with
// Stream::readBytes(), and on core 3.x NetworkClient::readBytes() gives up on
// the FIRST -1 from read() - only a 0 makes it wait. NetworkClientSecure::read()
// returns -1 whenever no decrypted byte is waiting, i.e. each time the parser
// catches up with the network at a TLS record boundary: the socket is
// non-blocking, the TCP window is small and mbedTLS fetches the next record
// only once the current one is used up. Any hiccup longer than the parse of
// one record therefore ended the body there and then - IncompleteInput on a
// 200, no core log line, and neither setTimeout(15000) ever consulted. Core
// 2.x (the 4") did wait, but only 1 s and as a busy spin: its
// WiFiClientSecure::setTimeout() never reaches Stream's 1000 ms default.
//
// So the waiting happens here, the same on both cores: data is handed out as
// soon as it is decrypted, otherwise the task sleeps a tick and asks again.
// The body is over only when the connection is gone with nothing left - the
// normal end, HTTP/1.0 closes after the body - or after BODY_IDLE_MS without
// a byte. A bare TCP FIN without a TLS close_notify does not show in
// connected() on either core (mbedTLS returns 0 for it, not an error), so
// such a close ends by the idle limit instead: 15 s later, but it ends.
// Deliberately NOT a Stream: ArduinoJson would then pick its Stream reader
// again, and the Stream base drags its own 1000 ms timeout along. As a plain
// class it gets the generic read()/readBytes() pair below. Reads go in
// 256-byte chunks - one mbedTLS read per chunk instead of two calls per byte -
// into a buffer that lives in the object on the 12 KB task stack.
class BshBodyReader {
public:
    explicit BshBodyReader(WiFiClientSecure &c) : client_(c), lastByteMs_(millis()) {}

    // The reader interface ArduinoJson 7 expects: one byte or -1 at the end,
    // and a block (only the MessagePack side uses that one).
    int read() {
        if (pos_ == len_ && !refill()) return -1;
        return buf_[pos_++];
    }
    size_t readBytes(char *dst, size_t n) {
        size_t got = 0;
        while (got < n) {
            if (pos_ == len_ && !refill()) break;
            size_t k = len_ - pos_;
            if (k > n - got) k = n - got;
            memcpy(dst + got, buf_ + pos_, k);
            pos_ += k; got += k;
        }
        return got;
    }
    // The next byte without taking it, -1 at the end: the body walker looks
    // before it hands a feature to ArduinoJson, opening brace still unread.
    int peek() {
        if (pos_ == len_ && !refill()) return -1;
        return buf_[pos_];
    }

    uint32_t bytes() const         { return bytes_; }
    uint32_t lastByteMs() const    { return lastByteMs_; }  // body start while none came
    uint32_t waits() const         { return waits_; }
    uint32_t longestWaitMs() const { return longestWaitMs_; }

private:
    // Next chunk into buf_; false = the body is over (see above).
    bool refill() {
        bool waiting = false; uint32_t waitStart = 0;
        for (;;) {
            int r = client_.read(buf_, sizeof(buf_));   // -1: nothing decrypted yet
            if (r > 0) {
                uint32_t now = millis();
                if (waiting && now - waitStart > longestWaitMs_) longestWaitMs_ = now - waitStart;
                pos_ = 0; len_ = (size_t)r;
                bytes_ += (uint32_t)r; lastByteMs_ = now;
                return true;
            }
            // connected() is what tells "not yet" from "never": it lets
            // mbedTLS look at the socket, and a close_notify, a reset or a
            // failed read turns it false. Data already decrypted is read
            // first, so nothing is left behind when it does.
            if (!client_.connected()) return false;
            if (millis() - lastByteMs_ >= BODY_IDLE_MS) return false;
            if (!waiting) { waiting = true; waitStart = millis(); waits_++; }
            vTaskDelay(1);
        }
    }

    WiFiClientSecure &client_;
    uint8_t  buf_[256];
    size_t   pos_ = 0, len_ = 0;
    uint32_t bytes_ = 0, lastByteMs_, waits_ = 0, longestWaitMs_ = 0;
};

// ── Body walker ──────────────────────────────────────────────────────────────
// fetchBsh() publishes ONE station out of the answer - the nearest usable
// gauge, its label, its datum and its next events - and every station can be
// judged, and the winner's data copied out, as soon as its own feature is
// complete. So only one feature at a time becomes a tree. The walker steps
// through the outer document itself,
//     {"type":..,"numberReturned":..,"features":[ {..}, {..}, .. ],"links":[..]}
// hands each feature object, opening brace still unread, to deserializeJson()
// with the filter, lets bshConsider() take what it needs into a BshStation on
// the stack, and the next deserializeJson() clears that tree again. Peak: the
// filter (168 B) plus the largest single filtered feature - two 1 KB pools and
// ~35 strings, 3.4 KB in all for the saved bodies - instead of all six
// stations at once. It grows with ONE station's event list only.
//
// It reads the outer document the way ArduinoJson reads it - the same white
// space, ArduinoJson's skipping rules for the values it passes over, the
// nesting limit counted from the outer document, the same error codes - so a
// body that is cut, broken or empty fails here as it failed there, and
// fetchBsh() publishes nothing from it. The one thing not taken in the outer
// layer is an ArduinoJson extra, not JSON: a member name without quotes.
// The escape table and the hex-digit decoding in bshReadString() are adapted
// from ArduinoJson (EscapeSequence.hpp, JsonDeserializer.hpp decodeHex()),
// MIT License, Copyright © 2014-2026, Benoit BLANCHON - see
// THIRD-PARTY-NOTICES.md and LICENSES/MIT-ArduinoJson.txt.

// The nesting limit of the WHOLE document, as ArduinoJson counts it; the
// walker itself takes the outer object (1) and the features array (2), so a
// feature is parsed with two levels less.
static constexpr uint8_t  BSH_NESTING      = ARDUINOJSON_DEFAULT_NESTING_LIMIT;
// Events kept for the winning station. Six get published
// (DataModel::MAX_TIDE_FC); the BSH sends ~23 per station, six days' worth,
// so 32 hold a whole list - past events included, should the clock still be
// unset when the walk begins.
static constexpr int      BSH_EVENTS_KEPT  = 32;
// The six are picked only after the body, against the clock as it is then
// (fetchBsh()). An event can be left out while walking only if it could
// never be picked: more than the 30 min allowed there in the past already
// when the walk began - plus this margin, for a clock stepped back meanwhile.
// Generous on purpose: with ~23 events per station and 32 slots it costs
// nothing on real data.
static constexpr uint32_t BSH_KEEP_SLACK_S = 12 * 3600;

struct BshEvent {
    uint32_t unixUtc;
    int      fv;                    // cm above gauge zero
    bool     isHigh;
};
// The nearest usable station so far, with all fetchBsh() publishes from it.
struct BshStation {
    double   dd         = 1e30;     // squared distance, see bshConsider()
    bool     found      = false;
    int      chartDatum = 0;        // cm, gauge zero -> chart datum
    char     label[40]  = {0};      // transliterated, as published
    int      evCount    = 0;
    BshEvent ev[BSH_EVENTS_KEPT];
};

// The height of one HW/NW event in cm above gauge zero; false if it has none.
// forecast_value exists only inside the official forecast horizon; past it the
// API sends the astronomical tidal_prediction_value, and as a STRING. Test for
// presence rather than treating 0 as "absent" - 0 cm is a legal reading, the
// water standing exactly at gauge zero.
static bool bshEventHeight(JsonObject e, int &fv) {
    if (e["forecast_value"].is<float>())
        fv = (int)lroundf(e["forecast_value"].as<float>());
    else if (e["tidal_prediction_value"].is<const char *>())
        fv = atoi(e["tidal_prediction_value"].as<const char *>());
    else if (e["tidal_prediction_value"].is<float>())
        fv = (int)lroundf(e["tidal_prediction_value"].as<float>());
    else return false;
    return true;
}

// One feature, while its tree exists. Nearest USABLE station (equirectangular
// squared distance). Usability is part of the ranking, not a check on the
// winner: several gauges carry no chart datum at all (Meldorf and Neuwerk
// have sent null), and electing one of those only to give up afterwards
// would hide a complete gauge a few kilometres away. Of two equally distant
// stations the first one stays.
static void bshConsider(JsonObject f, float lat, float lon, uint32_t walkStartUtc,
                        BshStation &st) {
    const float D2R = 0.01745329f;
    JsonArray c = f["geometry"]["coordinates"];
    if (c.isNull() || c.size() < 2) return;
    JsonObject pr = f["properties"];
    // The datum arrives as a JSON FLOAT (313.0) and sometimes as null. It must
    // be read as a float: ArduinoJson's is<int>() is false for a float
    // variant, so the old "| 0" default won on EVERY station and every height
    // was published uncorrected - 302 cm too high at Cuxhaven.
    if (!pr["chartdatum_relative_to_gaugezero"].is<float>()) return;
    JsonArray evs = pr["high_water_low_water"].as<JsonArray>();
    if (evs.isNull() || evs.size() == 0) return;
    double flon = c[0].as<double>(), flat = c[1].as<double>();
    double dx = (flon - lon) * cos(lat * D2R), dy = (flat - lat);
    double dd = dx * dx + dy * dy;
    if (!(dd < st.dd)) return;                            // NaN never wins either
    st.dd         = dd;
    st.found      = true;
    st.chartDatum = (int)lroundf(pr["chartdatum_relative_to_gaugezero"].as<float>());
    translit(pr["gauge_label"] | "", st.label, sizeof(st.label));
    // Its events, in order, less those that can never be among the six
    // (BSH_KEEP_SLACK_S): no time, past for good, or no height.
    st.evCount = 0;
    for (JsonObject e : evs) {
        uint32_t u = parseBshTime(e["event_timestamp"] | (const char *)nullptr);
        if (!u || (uint64_t)u + 1800 + BSH_KEEP_SLACK_S < walkStartUtc) continue;
        int fv;
        if (!bshEventHeight(e, fv)) continue;             // no height at all
        if (st.evCount == BSH_EVENTS_KEPT) break;
        const char *ev = e["event"] | "";
        st.ev[st.evCount++] = { u, fv, ev[0] == 'H' || ev[0] == 'h' };
    }
}

// What ArduinoJson takes for the end of the input: no byte, or a NUL.
static bool bshAtEnd(int c) { return c <= 0; }

// The next byte that is not white space, left unread.
static int bshPeekToken(BshBodyReader &in) {
    int c;
    while (!bshAtEnd(c = in.peek()) && (c == ' ' || c == '\t' || c == '\r' || c == '\n'))
        in.read();
    return c;
}

// The error for byte c where something else belonged.
static DeserializationError::Code bshUnexpected(int c) {
    return bshAtEnd(c) ? DeserializationError::IncompleteInput
                       : DeserializationError::InvalidInput;
}

// A quoted string, from its opening quote on. Without 'out' it is skipped,
// escapes unchecked, as ArduinoJson skips a value. With 'out' it is a member
// name: escapes are decoded and refused exactly as ArduinoJson decodes and
// refuses them in a name, and the name is kept - or comes back empty when it
// does not fit or holds anything beyond ASCII, as it is then never one the
// walker looks for.
static DeserializationError::Code bshReadString(BshBodyReader &in,
                                                char *out = nullptr, size_t cap = 0) {
    static const char UNESCAPE[] = "//''\"\"\\\\b\bf\fn\nr\rt\t";   // ArduinoJson's table (MIT, above)
    const int quote = in.read();
    size_t n = 0; bool whole = true;
    for (;;) {
        int c = in.read();
        if (bshAtEnd(c)) return DeserializationError::IncompleteInput;
        if (c == quote) break;
        if (c == '\\') {
            c = in.read();
            if (bshAtEnd(c)) return DeserializationError::IncompleteInput;
            if (!out) continue;
            if (c == 'u') {
                unsigned v = 0;
                for (int i = 0; i < 4; i++) {
                    int h = in.read();
                    if (bshAtEnd(h)) return DeserializationError::IncompleteInput;
                    unsigned d = (unsigned)(h < 'A' ? h - '0' : (h & ~0x20) - 'A' + 10);
                    if (d > 0x0F) return DeserializationError::InvalidInput;
                    v = (v << 4) | d;
                }
                if (v >= 0xD800 && v < 0xDC00) continue;  // high surrogate: adds nothing yet
                if (v == 0 || v >= 0x80) { whole = false; continue; }
                c = (int)v;
            } else {
                const char *p = UNESCAPE;
                while (*p && *p != c) p += 2;
                if (!*p) return DeserializationError::InvalidInput;
                c = (unsigned char)p[1];
            }
        }
        if (n + 1 < cap) out[n++] = (char)c;
        else             whole = false;
    }
    if (out) out[whole ? n : 0] = 0;
    return DeserializationError::Ok;
}

// Skips one value of any kind, as ArduinoJson skips a filtered-out one;
// 'nesting' is what is left of the nesting limit at this depth.
static DeserializationError::Code bshSkipValue(BshBodyReader &in, uint8_t nesting) {
    int c = bshPeekToken(in);
    if (bshAtEnd(c)) return DeserializationError::IncompleteInput;
    if (c == '"' || c == '\'') return bshReadString(in);
    if (c == 't' || c == 'f' || c == 'n') {
        for (const char *kw = c == 't' ? "true" : c == 'f' ? "false" : "null"; *kw; kw++) {
            c = in.peek();
            if (bshAtEnd(c)) return DeserializationError::IncompleteInput;
            if (c != *kw)    return DeserializationError::InvalidInput;
            in.read();
        }
        return DeserializationError::Ok;
    }
    if (c == '{' || c == '[') {
        if (nesting == 0) return DeserializationError::TooDeep;
        const bool isObject = (c == '{');
        const int  close    = isObject ? '}' : ']';
        in.read();
        c = bshPeekToken(in);
        if (isObject && c == '}') { in.read(); return DeserializationError::Ok; }
        for (;;) {
            if (isObject) {
                if (c != '"' && c != '\'') return bshUnexpected(c);
                DeserializationError::Code e = bshReadString(in);
                if (e) return e;
                if ((c = bshPeekToken(in)) != ':') return bshUnexpected(c);
                in.read();
            }
            DeserializationError::Code e = bshSkipValue(in, nesting - 1);
            if (e) return e;
            c = bshPeekToken(in);
            if (c == close) { in.read(); return DeserializationError::Ok; }
            if (c != ',') return bshUnexpected(c);
            in.read();
            c = bshPeekToken(in);
        }
    }
    // Anything else runs as far as a number could - ArduinoJson's rule, which
    // also lets an empty one through for the separator check to refuse.
    while ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E') {
        in.read();
        c = in.peek();
    }
    return DeserializationError::Ok;
}

// The features array, element by element. A second "features" member would
// replace the first, as it does in a tree.
static DeserializationError::Code bshWalkFeatures(BshBodyReader &in, JsonDocument &doc,
                                                  JsonDocument &filter, float lat, float lon,
                                                  uint32_t walkStartUtc, BshStation &st,
                                                  int &features) {
    st = BshStation();
    features = 0;
    int c = bshPeekToken(in);
    if (c != '[') return bshSkipValue(in, BSH_NESTING - 1);    // no list, no stations
    in.read();
    c = bshPeekToken(in);
    if (c == ']') { in.read(); return DeserializationError::Ok; }
    for (;;) {
        features++;
        if (c == '{') {
            DeserializationError err =
                deserializeJson(doc, in, DeserializationOption::Filter(filter),
                                DeserializationOption::NestingLimit(BSH_NESTING - 2));
            if (err) return err.code();
            bshConsider(doc.as<JsonObject>(), lat, lon, walkStartUtc, st);
        } else {
            // Not an object: an element all the same, but never a station.
            DeserializationError::Code e = bshSkipValue(in, BSH_NESTING - 2);
            if (e) return e;
        }
        c = bshPeekToken(in);
        if (c == ']') { in.read(); return DeserializationError::Ok; }
        if (c != ',') return bshUnexpected(c);
        in.read();
        c = bshPeekToken(in);
    }
}

// The whole body. Ok only once the outer object has closed, i.e. the body is
// complete; until then 'st' holds no more than a guess.
static DeserializationError bshWalk(BshBodyReader &in, JsonDocument &doc, JsonDocument &filter,
                                    float lat, float lon, uint32_t walkStartUtc,
                                    BshStation &st, int &features) {
    features = 0;
    int c = bshPeekToken(in);
    if (bshAtEnd(c)) return DeserializationError::EmptyInput;
    // Valid JSON, but not an object: no features in it - as before.
    if (c != '{') return bshSkipValue(in, BSH_NESTING);
    in.read();
    c = bshPeekToken(in);
    if (c == '}') { in.read(); return DeserializationError::Ok; }
    for (;;) {
        if (c != '"' && c != '\'') return bshUnexpected(c);
        char key[12];
        DeserializationError::Code e = bshReadString(in, key, sizeof(key));
        if (e) return e;
        if ((c = bshPeekToken(in)) != ':') return bshUnexpected(c);
        in.read();
        if (strcmp(key, "features") == 0)
            e = bshWalkFeatures(in, doc, filter, lat, lon, walkStartUtc, st, features);
        else
            e = bshSkipValue(in, BSH_NESTING - 1);
        if (e) return e;
        c = bshPeekToken(in);
        if (c == '}') { in.read(); return DeserializationError::Ok; }
        if (c != ',') return bshUnexpected(c);
        in.read();
        c = bshPeekToken(in);
    }
}

// ── Fetch + parse ────────────────────────────────────────────────────────────
// What a fetch came back with. Only Failed is worth asking again soon (see
// RETRY_MS); NoData is the server's own answer - no gauge in the box, none
// usable, nothing ahead. Neither of the two touches the stored forecast.
enum class BshFetch : uint8_t { Published, NoData, Failed };

static BshFetch fetchBsh() {
    float lat, lon; bool gpsOk;
    {
        auto lk = data.lock();
        // The last position ever known, not the current one: lat/lon go blank
        // 10 s after the GPS stops (BusWatch.h), and the gauges near the boat
        // are still the right ones then - the fallback region would not be.
        lat = data.lastKnownLat; lon = data.lastKnownLon;
        gpsOk = !isnan(lat) && !isnan(lon);
    }
    if (!gpsOk) { lat = FALLBACK_LAT; lon = FALLBACK_LON; }
    bool roundPos = gpsOk;
#if defined(BSH_BENCH_POS)
    lat = BENCH_LAT; lon = BENCH_LON;   // bench build: always the same answer (above)
    roundPos = false;
#endif
    // The query names the boat's position only to 0.1 degree (11 km north-
    // south, about 6.5 km east-west at 54 N): it leaves the device with the
    // IP address every 3 h. The gauge is still chosen on the real position
    // (bshWalk() gets lat/lon, not qLat/qLon). Rounding moves the box's centre
    // by up to 0.05 degree per axis, so the box still reaches at least 0.30
    // degree from the boat in every direction (33 km north-south, 19.6 km
    // east-west at 54 N) instead of 0.35: only a nearest gauge farther out
    // than that on one axis can now be missed. The fixed positions (Cuxhaven,
    // the bench) are nobody's whereabouts and go out as before.
    const float qLat = roundPos ? roundf(lat * 10.0f) / 10.0f : lat;
    const float qLon = roundPos ? roundf(lon * 10.0f) / 10.0f : lon;

    const float d = 0.35f;       // bbox half-size in degrees
    char url[320];
    snprintf(url, sizeof(url),
             "%s?lang=de&bbox=%.3f,%.3f,%.3f,%.3f&limit=6&f=json",
             BSH_URL_BASE, qLon - d, qLat - d, qLon + d, qLat + d);
    Serial.printf("[BSH] GET %s\n", url);

    WiFiClientSecure client; client.setInsecure();   // public read-only data
    client.setTimeout(15000);
    HTTPClient http;
    if (!http.begin(client, url)) {
        Serial.println("[BSH] http.begin failed");
#if BSH_BENCH
        bshBenchLine("failed", "http.begin failed", 0, 0, 0, BshBenchHeap());
#endif
        return BshFetch::Failed;
    }
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.useHTTP10(true);     // disable chunked transfer so the JSON streams cleanly
    http.addHeader("Accept", "application/json");
    http.setUserAgent(BSH_USER_AGENT);   // addHeader() refuses User-Agent on both cores
    http.setTimeout(15000);
    const uint32_t fetchStartMs = millis();   // handshake + headers + body
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[BSH] HTTP %d\n", code);
#if BSH_BENCH
        BshBenchHeap benchHeap; benchHeap.take();
        const uint32_t benchMs = millis() - fetchStartMs;
#endif
        http.end();
#if BSH_BENCH
        char why[24]; snprintf(why, sizeof(why), "HTTP %d", code);
        bshBenchLine("failed", why, 0, 0, benchMs, benchHeap);
#endif
        return BshFetch::Failed;
    }

    // Filter, per feature: what bshConsider() reads; the huge per-station
    // 'curve' is skipped. It can only be skipped, not avoided: the service
    // rejects both ?properties= and ?skipGeometry= with 400 (verified against
    // the live API), so the whole 433 KB body still has to travel through the
    // parser.
    BSH_JSON_DOC(filter);
    filter["geometry"]["coordinates"] = true;
    filter["properties"]["gauge_label"] = true;
    filter["properties"]["chartdatum_relative_to_gaugezero"] = true;
    filter["properties"]["high_water_low_water"][0]["event"] = true;
    filter["properties"]["high_water_low_water"][0]["event_timestamp"] = true;
    filter["properties"]["high_water_low_water"][0]["forecast_value"] = true;
    filter["properties"]["high_water_low_water"][0]["tidal_prediction_value"] = true;
    // A filter cut short by a failed allocation would still filter - just
    // differently: without forecast_value, say, the astronomical heights
    // would be published as the forecast.
    if (filter.overflowed()) {
        Serial.println("[BSH] no memory for the JSON filter");
#if BSH_BENCH
        BshBenchHeap benchHeap; benchHeap.take();
        const uint32_t benchMs = millis() - fetchStartMs;
#endif
        http.end();
#if BSH_BENCH
        bshBenchLine("failed", "no memory for the JSON filter", 0, 0, benchMs, benchHeap);
#endif
        return BshFetch::Failed;
    }

    BSH_JSON_DOC(doc);                 // one feature's tree at a time (bshWalk)
    // Wraps the client itself, not http.getStream(): that is the same object
    // while connected, but handed out as WiFiClient& on core 2.x and as
    // NetworkClient& on 3.x, whereas 'client' is a WiFiClientSecure on both.
    BshBodyReader body(client);
    BshStation st;                     // with its temporaries ~0.75 KB of the 12 KB stack
    int features = 0;
    s_bshJson.peak = s_bshJson.live;   // the filter, so far
    DeserializationError err = bshWalk(body, doc, filter, lat, lon,
                                       (uint32_t)time(nullptr), st, features);
    const uint32_t parseEndMs = millis();
    // A partial body is NEVER consumed. IncompleteInput means the body ran out
    // mid-document - the connection closed early (the server cut it, a reset)
    // or BODY_IDLE_MS passed without a byte, the only two ends BshBodyReader
    // knows - and what the walk had gathered by then is a guess: the nearest
    // of the stations that happened to arrive, or one whose event list simply
    // stops. Publishing that would put a wrong "next tide" on the clock. Keep
    // the previous forecast instead; the ClockScreen ages it out by itself
    // after 12 h. The error line tells the causes apart: bytes against
    // Content-Length shows where it stopped; "last byte" near 15 s with the
    // link still up is the idle limit (a frozen STA path, a stalled server, or
    // a bare FIN), a short one with the link closed is the far end hanging up;
    // the heap figures cover the old DRAM starvation and PSRAM shortage (top
    // of this file).
    if (err) {
        // All sampled BEFORE http.end(), which stops the client and forgets
        // the Content-Length - and before the client is torn down, so the
        // heap is as the transfer left it.
        const int      contentLen = http.getSize();          // -1 = not sent
        const bool     linkUp     = client.connected();
        const unsigned heapFree   = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const unsigned heapBlock  = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        const unsigned psramFree  = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const unsigned psramBlock = (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
#if BSH_BENCH
        BshBenchHeap benchHeap; benchHeap.take();
#endif
        http.end();
        Serial.printf("[BSH] JSON err: %s (body %lu B, Content-Length %d, %lu ms total, "
                      "last byte %lu ms ago, %s; internal heap free %u B, largest block %u B; "
                      "PSRAM free %u B, largest block %u B; JSON peak %u B)\n",
                      err.c_str(), (unsigned long)body.bytes(), contentLen,
                      (unsigned long)(parseEndMs - fetchStartMs),
                      (unsigned long)(parseEndMs - body.lastByteMs()),
                      linkUp ? "still connected" : "closed", heapFree, heapBlock,
                      psramFree, psramBlock, (unsigned)s_bshJson.peak);
#if BSH_BENCH
        char why[40]; snprintf(why, sizeof(why), "JSON %s", err.c_str());
        bshBenchLine("failed", why, features, body.bytes(), parseEndMs - fetchStartMs, benchHeap);
#endif
        return BshFetch::Failed;
    }
#if BSH_BENCH
    BshBenchHeap benchHeap; benchHeap.take();
    const uint32_t benchBytes = body.bytes(), benchMs = parseEndMs - fetchStartMs;
#endif
    http.end();
    // A wait is exactly the situation in which the old getStream() path ended
    // the body on core 3.x; count and longest show the slack the reader buys.
    // The JSON peak is the filter plus the largest feature's tree.
    Serial.printf("[BSH] body %lu B in %lu ms, %lu waits for data (longest %lu ms), "
                  "%d stations, JSON peak %u B\n",
                  (unsigned long)body.bytes(), (unsigned long)(parseEndMs - fetchStartMs),
                  (unsigned long)body.waits(), (unsigned long)body.longestWaitMs(),
                  features, (unsigned)s_bshJson.peak);

    if (features == 0) {
        Serial.println("[BSH] no stations in bbox");
#if BSH_BENCH
        bshBenchLine("nodata", "no stations in bbox", features, benchBytes, benchMs, benchHeap);
#endif
        return BshFetch::NoData;
    }
    if (!st.found) {
        Serial.println("[BSH] no usable station");
#if BSH_BENCH
        bshBenchLine("nodata", "no usable station", features, benchBytes, benchMs, benchHeap);
#endif
        return BshFetch::NoData;
    }

    // The six to publish, against the clock as it is now that the body is in.
    uint32_t nowUtc = (uint32_t)time(nullptr);
    DataModel::TideExtreme tmp[DataModel::MAX_TIDE_FC];
    int n = 0; uint32_t lastU = 0;
    for (int i = 0; i < st.evCount; i++) {
        if (n >= DataModel::MAX_TIDE_FC) break;
        const BshEvent &e = st.ev[i];
        uint32_t u = e.unixUtc;
        if (u + 1800 < nowUtc) continue;                  // skip events >30 min past
        if (u <= lastU) continue;                         // the ClockScreen takes the
                                          // FIRST future entry as "next tide", so the
                                          // published list has to be strictly ascending
        long cm = (long)e.fv - st.chartDatum;
        if (cm < -3000 || cm > 3000) continue;            // +/-30 m is not a gauge reading
        tmp[n].unixUtc = u;
        tmp[n].cmCD    = (int16_t)cm;                     // cm above chart datum
        tmp[n].isHigh  = e.isHigh;
        lastU = u;
        n++;
    }
    if (n == 0) {
        Serial.println("[BSH] station has no upcoming events");
#if BSH_BENCH
        bshBenchLine("nodata", "station has no upcoming events", features, benchBytes, benchMs,
                     benchHeap);
#endif
        return BshFetch::NoData;
    }

    {
        auto lk = data.lock();
        for (int i = 0; i < n; i++) data.tideFc[i] = tmp[i];
        data.tideFcCount = n;
        strncpy(data.tideStation, st.label, sizeof(data.tideStation) - 1);
        data.tideStation[sizeof(data.tideStation) - 1] = 0;
        data.tideIsBsh    = true;
        data.lastTideFcMs = millis();
    }
    Serial.printf("[BSH] '%s' chartDatum=%dcm events=%d:\n", st.label, st.chartDatum, n);
    for (int i = 0; i < n; i++)
        Serial.printf("[BSH]   %s u=%lu  %.2fm CD\n", tmp[i].isHigh ? "HW" : "NW",
                      (unsigned long)tmp[i].unixUtc, tmp[i].cmCD / 100.0);
#if BSH_BENCH
    bshBenchLine("published", st.label, features, benchBytes, benchMs, benchHeap);
#endif
    return BshFetch::Published;
}

// ── Background task ──────────────────────────────────────────────────────────
static void bshTask(void *) {
    bool synced = false;
    int  iter   = 0;
    // Seeded from the current setting so a device that BOOTS with the fetch
    // off does not announce a "switched off" it never switched away from.
    bool wasFetching = appConfig.cfg.tideBshFetch;
    bool wifiMissLogged = false;   // one line per outage, not one per minute
    // Retry state (RETRY_MS). slotAtMs is when the current period runs out,
    // i.e. when the next slot falls due; retries only fill the time before it.
    bool     retryNext = false;    // the coming pass is a retry, not a slot
    int      retries   = 0;        // retries made in the current outage
    uint32_t slotAtMs  = 0;
    // Let boot settle first: screen creation + WiFi association allocate heap, and
    // the TLS fetch (~40 KB transient) must not collide with that peak.
    vTaskDelay(pdMS_TO_TICKS(25000));
    for (;;) {
        // A retry still waiting for WiFi when its period runs out has become
        // the slot: it runs as one, counts as one and opens the next period.
        if (retryNext && (int32_t)(millis() - slotAtMs) >= 0) retryNext = false;
        uint32_t waitMs = FETCH_PERIOD_MS;
        if (WiFi.status() == WL_CONNECTED) {
            wifiMissLogged = false;
            // SNTP runs REGARDLESS of the tide setting. This task carries two
            // unrelated jobs, and for a device with no GPS on the bus the clock
            // sync is the only time source there is - switching the tide
            // forecast off must not take the clock with it, or the clock page
            // loses the date, the sun and moon lines, and its own astronomical
            // tide estimate along with them. A retry syncs only a clock that
            // never synced; the ~24 h resync belongs to the slots.
            if (!synced || (!retryNext && (iter % RESYNC_EVERY) == 0)) {
                if (syncTimeFromSntp()) synced = true;
            }

            bool fetchFailed = false;
            if (appConfig.cfg.tideBshFetch) {
                fetchFailed = (fetchBsh() == BshFetch::Failed);
            } else if (wasFetching) {
                // Switched off while running. Drop what was already stored, or
                // the clock page would keep presenting an ageing forecast as
                // current for up to twelve hours - its freshness window - while
                // the user believes the source is off. Falls through to PGN
                // 130320 or the astronomical estimate on the next repaint.
                {
                    auto lk = data.lock();
                    for (int i = 0; i < DataModel::MAX_TIDE_FC; i++)
                        data.tideFc[i] = DataModel::TideExtreme{};
                    data.tideFcCount = 0;
                    data.tideIsBsh   = false;
                    data.lastTideFcMs = 0;
                    memset(data.tideStation, 0, sizeof(data.tideStation));
                }
                Serial.println("[BSH] fetch disabled in config - stored forecast cleared");
            }
            wasFetching = appConfig.cfg.tideBshFetch;
            // The 12 KB stack is sized for the TLS handshake and lives in PSRAM;
            // print what was left so a future mbedTLS/ArduinoJson change cannot
            // quietly eat the margin without anyone seeing it.
            Serial.printf("[BSH] stack low-water %u B, internal heap free %u B\n",
                          (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

#if defined(BSH_BENCH_EVERY_S)
            // Bench build: every pass is a slot, BENCH_PERIOD_MS after the
            // last, whatever it came back with (top of this file). iter still
            // counts the passes for the SNTP resync.
            iter++;
            waitMs = BENCH_PERIOD_MS;
            (void)fetchFailed; (void)retries;
#else
            // Next pass. A slot opens a new period; a retry only uses it up.
            const bool     wasRetry = retryNext;
            const uint32_t now      = millis();
            retryNext = false;
            if (wasRetry) retries++;
            else        { iter++; slotAtMs = now + FETCH_PERIOD_MS; }
            // What is left of the period: all of it after a slot. Never less
            // than the first retry gap, or a retry that waited for WiFi until
            // just before the slot would see the slot's own 433 KB GET follow
            // it within seconds.
            uint32_t leftMs = (int32_t)(slotAtMs - now) > 0 ? slotAtMs - now : 0;
            if (leftMs < RETRY_MS[0]) leftMs = RETRY_MS[0];
            waitMs = leftMs;
            if (!fetchFailed) {
                retries = 0;        // an answer - or no fetch at all - ends the outage
            } else if (retries < RETRY_COUNT) {
                if (RETRY_MS[retries] < leftMs) {
                    retryNext = true;
                    waitMs    = RETRY_MS[retries];
                    Serial.printf("[BSH] fetch failed - retrying in %lu min\n",
                                  (unsigned long)(waitMs / 60000UL));
                } else {
                    // Only when a retry had to wait long for WiFi. The slot
                    // takes this retry's place, and a failing slot is
                    // retried in turn - still two extras at most.
                    Serial.println("[BSH] fetch failed - the next 3-h slot comes first");
                }
            } else {
                Serial.println("[BSH] fetch failed - giving up until the next 3-h slot");
            }
#endif
        } else {
            // Not a pass: iter stays, so the ~24 h SNTP resync still counts
            // regular fetch periods, and a slot is never less than a full
            // period after the previous one. A pending retry waits here too,
            // at most until its period runs out (top of the loop).
            if (!wifiMissLogged)
                Serial.println("[BSH] WiFi not connected, checking again every 60 s");
            wifiMissLogged = true;
            waitMs = WIFI_RECHECK_MS;
        }
        bshSleep(waitMs, synced);      // and keeps an eye on SNTP meanwhile (see there)
    }
}

void bshTideBegin() {
    // Fixed strings, so tools/release_checks.py can refuse such an image.
#if defined(BSH_BENCH_POS)
    Serial.println("[BSH] TEST BUILD BSH_BENCH_POS - not for release (every request for "
                   "53.870 N 8.720 E)");
#endif
#if defined(BSH_BENCH_EVERY_S)
    Serial.printf("[BSH] TEST BUILD BSH_BENCH_EVERY_S - not for release (each fetch %lu s "
                  "after the last, no retries)\n", (unsigned long)(BSH_BENCH_EVERY_S));
#endif
#if ESP_IDF_VERSION_MAJOR >= 5
    // 12 KB of stack for a task that does one HTTPS fetch every 3 hours is
    // the single largest firmware-owned DRAM block. On IDF 5 the stack can
    // live in PSRAM (sdkconfig already has SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    // and FREERTOS_TASK_CREATE_ALLOW_EXT_MEM enabled): legal because this
    // task never writes flash and runs nothing ISR-ish - it fetches, parses,
    // and fills the DataModel. TLS itself is PSRAM-backed anyway (mbedTLS
    // EXTERNAL_MEM_ALLOC).
    if (xTaskCreatePinnedToCoreWithCaps(bshTask, "BSH", 12288, nullptr, 1,
                                        nullptr, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
            != pdPASS) {
        // PSRAM exhausted? Fall back rather than silently losing the feature.
        xTaskCreatePinnedToCore(bshTask, "BSH", 12288, nullptr, 1, nullptr, 1);
    }
#else
    xTaskCreatePinnedToCore(bshTask, "BSH", 12288, nullptr, 1, nullptr, 1);
#endif
    Serial.println("[setup] BSH tide task started");
}
