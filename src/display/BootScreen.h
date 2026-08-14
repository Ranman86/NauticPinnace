#pragma once
#include <lvgl.h>
#include "Theme.h"

// ============================================================
// BootScreen – animated boot screen with optional custom logo.
//
// Animation sequence (built-in, no files required):
//   0 ms  : rotating spinner + dark background
//   0-500 : title + boat name fade in from below
//   500+  : progress bar + status text appear
//
// Custom logo (optional):
//   Upload /logo.bin via the web UI → Settings → Boot-Logo.
//   Format: raw RGB565 little-endian with a 4-byte header:
//     uint16_t width, uint16_t height, then (w*h*2) bytes.
//   Max recommended size: 200×200 px (78 KB).
//   See tools/logo_convert.py for conversion.
// ============================================================
class BootScreen {
public:
    void show(const char *boatName = "");
    void update(const char *statusMsg, uint8_t progress = 0);
    void dismiss();

    // Pump LVGL manually (call while tasks are not yet running)
    static void tick(uint32_t ms = 16);

private:
    lv_obj_t *_scr       = nullptr;
    lv_obj_t *_bar       = nullptr;
    lv_obj_t *_statusLbl = nullptr;
    lv_obj_t *_pctLbl    = nullptr;
    lv_obj_t *_spinner   = nullptr;
    lv_obj_t *_logoImg   = nullptr;   // custom logo canvas

    // Raw pixel buffer for custom logo (PSRAM)
    lv_color_t   *_logoBuf = nullptr;
    lv_img_dsc_t  _logoDesc{};

    void playIntro(const char *boatName);
    bool loadCustomLogo();             // returns true if /logo.bin found & loaded

    // Built-in NauticPi mark (pi over a wave), drawn with LVGL lines when no
    // /logo.bin is present. Returns the container, positioned by the caller.
    lv_obj_t *buildMark(lv_obj_t *parent, int size);
};

extern BootScreen bootScreen;
