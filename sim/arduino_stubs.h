// ============================================================
// arduino_stubs.h  –  Arduino / ESP32 API stubs for PC simulator
// ============================================================
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <string>
#include "sdl2_wrapper.h"   // SDL2 with assertion suppressed

// ── Time ──────────────────────────────────────────────────────────────────────
inline uint32_t millis_stub() { return SDL_GetTicks(); }
#define millis()        millis_stub()
#define micros()        (SDL_GetTicks() * 1000u)
#ifndef delay
#define delay(ms)       SDL_Delay(ms)
#endif

// ── vTaskDelay (FreeRTOS stub) ────────────────────────────────────────────────
#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(ms) (ms)
#endif
#ifndef VTASKDELAY_DEFINED
#define VTASKDELAY_DEFINED
inline void vTaskDelay(uint32_t ticks) { SDL_Delay(ticks); }
#endif

// ── Serial ────────────────────────────────────────────────────────────────────
struct _SerialStub {
    void begin(int) {}
    operator bool() const { return true; }
    void print(const char *s)   { fputs(s, stdout); }
    void print(int v)           { printf("%d", v); }
    void print(float v)         { printf("%.2f", v); }
    void println(const char *s) { puts(s); }
    void println(int v)         { printf("%d\n", v); }
    void println(float v)       { printf("%.2f\n", v); }
    void println()              { puts(""); }
    void printf(const char *fmt, ...) {
        va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    }
    void flush() { fflush(stdout); }
};
inline _SerialStub Serial;

// ── Arduino String ────────────────────────────────────────────────────────────
struct String : public std::string {
    String() : std::string() {}
    String(const char *s) : std::string(s ? s : "") {}
    String(int v)         : std::string(std::to_string(v)) {}
    String(float v)       : std::string(std::to_string(v)) {}
    String(double v)      : std::string(std::to_string(v)) {}
    const char *c_str() const { return std::string::c_str(); }
    // Implicit conversion to const char* so String works where char* is expected
    operator const char*() const { return std::string::c_str(); }
    int    length()  const    { return (int)std::string::length(); }
    bool   isEmpty() const    { return empty(); }
    bool   startsWith(const char *p) const { return find(p) == 0; }
    String operator+(const char *s)  const { return String((std::string(*this) + s).c_str()); }
    String operator+(const String &s) const { return String((std::string(*this) + std::string(s)).c_str()); }
    // ArduinoJson serialization support
    size_t write(uint8_t c)            { push_back((char)c); return 1; }
    size_t write(const char *s, size_t n) { append(s, n); return n; }
    size_t write(const char *s)        { append(s); return strlen(s); }
};
inline String operator+(const char *a, const String &b) {
    return String((std::string(a) + std::string(b)).c_str());
}

// ── min / max / abs (Arduino style) ──────────────────────────────────────────
#ifndef min
template<typename T> inline T min(T a, T b) { return a < b ? a : b; }
template<typename T> inline T max(T a, T b) { return a > b ? a : b; }
#endif

// ── ESP32 heap stubs ──────────────────────────────────────────────────────────
#define MALLOC_CAP_INTERNAL  0x00001
#define MALLOC_CAP_SPIRAM    0x00800
#define MALLOC_CAP_DMA       0x00100
#define MALLOC_CAP_8BIT      0x00004

#include <cstdlib>
inline void *heap_caps_malloc(size_t size, uint32_t /*caps*/) { return malloc(size); }
inline size_t heap_caps_get_free_size(uint32_t /*caps*/)      { return 4*1024*1024; }
inline void   heap_caps_free(void *p)                         { free(p); }

// ── ESP task WDT stubs ────────────────────────────────────────────────────────
inline void esp_task_wdt_reset() {}
inline void disableLoopWDT()    {}
inline void disableCore0WDT()   {}
inline void disableCore1WDT()   {}

// ── LittleFS stub ─────────────────────────────────────────────────────────────
// File stub – returned by LittleFS.open(), Config uses it
struct File {
    operator bool() const { return false; }
    int    read(uint8_t*, size_t) { return 0; }
    size_t write(const uint8_t*, size_t) { return 0; }
    void   close() {}
    size_t size() { return 0; }
    bool   available() { return false; }
    String readString() { return String(""); }
    void   print(const char*) {}
    File   openNextFile() { return File{}; }
    const char* name() { return ""; }
};
// Keep old alias too
using _FileStub = File;
struct _LittleFSStub {
    bool begin(bool formatOnFail=false, const char* mountPt="/littlefs",
               uint8_t maxFiles=10, const char* label="littlefs") { return false; }
    bool exists(const char*) { return false; }
    bool remove(const char*) { return false; }
    File open(const char*, const char* mode="r", bool create=false) { return File{}; }
};
inline _LittleFSStub LittleFS;

// ── WiFi stub ─────────────────────────────────────────────────────────────────
struct _WiFiStub {
    struct _IP { String toString() { return String("192.168.4.1"); } } _ip;
    void  softAP(const char*, const char* =nullptr) {}
    void  begin(const char*, const char*) {}
    int   status() { return 3; }
    _IP   softAPIP()  { return _ip; }
    _IP   localIP()   { return _ip; }
};
inline _WiFiStub WiFi;

// ── ArduinoJson compatibility ─────────────────────────────────────────────────
// JsonDocument is included from the real lib, no stub needed

// ── DEG_TO_RAD / RAD_TO_DEG ───────────────────────────────────────────────────
#ifndef DEG_TO_RAD
#define DEG_TO_RAD (M_PI / 180.0)
#endif
#ifndef RAD_TO_DEG
#define RAD_TO_DEG (180.0 / M_PI)
#endif

// ── Arduino GPIO stubs ────────────────────────────────────────────────────────
#define INPUT         0x0
#define OUTPUT        0x1
#define INPUT_PULLUP  0x2
#define HIGH          0x1
#define LOW           0x0
inline void    pinMode(int, int)    {}
inline int     digitalRead(int)     { return HIGH; }
inline void    digitalWrite(int, int) {}
inline int     analogRead(int)      { return 0; }

// ── strlcpy (BSD function, not in MinGW by default) ───────────────────────────
#include <cstring>
inline size_t strlcpy(char *dst, const char *src, size_t size) {
    if (!size) return src ? strlen(src) : 0;
    size_t len = strlen(src);
    if (len >= size) len = size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
    return strlen(src);
}

// ── isnan / isinf ─────────────────────────────────────────────────────────────
using std::isnan;
using std::isinf;
using std::abs;
