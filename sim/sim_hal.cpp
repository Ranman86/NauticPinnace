// ============================================================
// sim_hal.cpp  –  SDL2 LVGL port for the NauticPinnace Simulator
// ============================================================

#include "sim_hal.h"
#include "arduino_stubs.h"
// SDL2 already included via sim_hal.h → sdl2_wrapper.h
#include <lvgl.h>
#include <cstdio>
#include <cstring>
#include "../src/display/DisplayManager.h"   // dispMgr: swipe navigation
#include "../src/Entropy.h"
#include "../src/display/UiConfig.h"         // UI_SWIPE_THRESHOLD, UI_SCREEN_W

// Presented frames. sim_main.cpp's getTickFps() consumes this; without it the
// perf overlay reported a permanent "0fps", which makes a merely slow simulator
// look hung.
uint32_t g_sim_frames = 0;

static SDL_Window   *s_window   = nullptr;
static SDL_Renderer *s_renderer = nullptr;
static SDL_Texture  *s_texture  = nullptr;
static int           s_width    = 0;
static int           s_height   = 0;

// ── Mouse state ──────────────────────────────────────────────────────────────
// LVGL samples the input device only from inside lv_timer_handler(), i.e. once
// per main-loop iteration. On the PC the canvas screens render slowly, so that
// gap was measured at 46 ms on average and up to 285 ms in the worst case.
// A naive "set s_pressed on DOWN, clear it on UP" therefore loses every click
// whose DOWN and UP land in the SAME SDL_PollEvent drain — they cancel out
// before LVGL ever looks. That made normal-speed clicks silently do nothing.
//
// Fix: queue the button transitions and hand LVGL exactly one per sample, so a
// press is always seen at least once before its release. Motion is not queued —
// while the queue is empty the live cursor position is reported, which keeps
// dragging (sliders, swipes) smooth.
static int  s_mx = 0, s_my = 0;
static bool s_pressed = false;          // state currently reported to LVGL

// Motion has to be queued as well, not just the button transitions. The main
// loop drains the WHOLE SDL queue before it calls lv_timer_handler(), so a drag
// that finishes inside one iteration delivers DOWN, every MOTION and UP in a
// single pass. If motion were merely applied to a live cursor position it would
// be overwritten by the queued UP and the drag would reach LVGL with zero
// travel — the swipe would never fire.
//
// Consecutive motions are COALESCED into the newest one, so a long drag cannot
// build up a backlog that replays in slow motion across later frames.
struct SimEvt { int x, y; bool pressed; bool motion; };
static SimEvt s_q[64];
static int    s_q_head = 0, s_q_tail = 0;
static bool   s_q_lastPressed = false;  // button state after everything queued

static inline bool sim_q_empty(void) { return s_q_head == s_q_tail; }
static inline int  sim_q_prev(int i)  { return (i - 1 + (int)(sizeof(s_q) / sizeof(s_q[0])))
                                               % (int)(sizeof(s_q) / sizeof(s_q[0])); }

static void sim_q_push(int x, int y, bool pressed, bool motion) {
    if (motion && !sim_q_empty()) {
        SimEvt &last = s_q[sim_q_prev(s_q_head)];
        if (last.motion) { last.x = x; last.y = y; return; }   // coalesce
    }
    int next = (s_q_head + 1) % (int)(sizeof(s_q) / sizeof(s_q[0]));
    if (next == s_q_tail) return;       // full: drop (never happens in practice)
    s_q[s_q_head] = { x, y, pressed, motion };
    s_q_head = next;
    if (!motion) s_q_lastPressed = pressed;
}

bool sim_hal_init(int width, int height, const char *title) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError());
        return false;
    }
    s_width  = width;
    s_height = height;

    s_window = SDL_CreateWindow(
        title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        width, height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!s_window) {
        fprintf(stderr, "SDL_CreateWindow error: %s\n", SDL_GetError());
        return false;
    }

    s_renderer = SDL_CreateRenderer(s_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_renderer) {
        fprintf(stderr, "SDL_CreateRenderer error: %s\n", SDL_GetError());
        return false;
    }

    // Pixel format matches LV_COLOR_DEPTH 16
    s_texture = SDL_CreateTexture(
        s_renderer,
        SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING,
        width, height);
    if (!s_texture) {
        fprintf(stderr, "SDL_CreateTexture error: %s\n", SDL_GetError());
        return false;
    }

    printf("SDL2 window created (%dx%d)\n", width, height);

    // SDL reports mouse coordinates in window space but SDL_RenderCopy stretches
    // the texture to the renderer output. Under Windows display scaling those two
    // can differ, which would offset every click. Warn instead of failing silently.
    {
        int ww = 0, wh = 0, ow = 0, oh = 0;
        SDL_GetWindowSize(s_window, &ww, &wh);
        SDL_GetRendererOutputSize(s_renderer, &ow, &oh);
        if (ww != ow || wh != oh)
            printf("[sim] WARNING: window %dx%d but render output %dx%d — "
                   "clicks will be offset (display scaling).\n", ww, wh, ow, oh);
    }
    return true;
}

void sim_hal_deinit(void) {
    if (s_texture)  { SDL_DestroyTexture(s_texture);   s_texture  = nullptr; }
    if (s_renderer) { SDL_DestroyRenderer(s_renderer); s_renderer = nullptr; }
    if (s_window)   { SDL_DestroyWindow(s_window);     s_window   = nullptr; }
    SDL_Quit();
}

void sim_hal_tick(void) {
    static uint32_t last_tick = 0;
    uint32_t now = SDL_GetTicks();
    lv_tick_inc(now - last_tick);
    last_tick = now;
}

void sim_hal_screenshot(const char *path) {
    if (!s_renderer || !s_texture) return;
    // Force a synchronous redraw FIRST. Without this the capture is one frame
    // stale: it copies the last PRESENTED texture, so a screenshot taken right
    // after an overlay opens shows the screen underneath it. At the sim's ~1 fps
    // that is the normal case, and it silently produced misleading evidence.
    lv_refr_now(nullptr);
    SDL_RenderCopy(s_renderer, s_texture, nullptr, nullptr);   // latest frame -> backbuffer
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, s_width, s_height, 32,
                                                       SDL_PIXELFORMAT_ARGB8888);
    if (!surf) return;
    SDL_RenderReadPixels(s_renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                         surf->pixels, surf->pitch);
    SDL_SaveBMP(surf, path);
    SDL_FreeSurface(surf);
    SDL_RenderPresent(s_renderer);
}

void sim_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    if (!s_texture) { lv_disp_flush_ready(drv); return; }

    int x1 = area->x1, y1 = area->y1;
    int w  = area->x2 - x1 + 1;
    int h  = area->y2 - y1 + 1;

    // Update only the dirty rectangle in the texture
    SDL_Rect rect = { x1, y1, w, h };
    SDL_UpdateTexture(s_texture, &rect, color_p, w * sizeof(lv_color_t));

    // Blit to screen when the last strip is done
    if (area->y2 >= s_height - 1) {
        SDL_RenderCopy(s_renderer, s_texture, nullptr, nullptr);
        SDL_RenderPresent(s_renderer);
        g_sim_frames++;             // feeds getTickFps() -> perf overlay
    }

    lv_disp_flush_ready(drv);
}

// ── Swipe tracker ────────────────────────────────────────────────────────────
// Mirrors the panel's tracker in DisplaySetup.cpp (lvgl_touch_cb). That file is
// hardware-only and excluded from the simulator build, so without this the sim
// had NO swipe navigation and never showed the nav arrows either — every click
// looked like it did nothing at all.
static bool s_swipe_active = false, s_swipe_done = false, s_swipe_suppress = false;
static int  s_swipe_start_x = 0, s_swipe_start_y = 0;
static int  s_swipe_last_x  = 0, s_swipe_last_y  = 0;

void swipeSuppress() { s_swipe_suppress = true; }

static void sim_track_swipe(bool pressed) {
    if (pressed) {
        s_swipe_last_x = s_mx;
        s_swipe_last_y = s_my;
        if (!s_swipe_active) {
            s_swipe_active   = true;
            s_swipe_done     = false;
            s_swipe_suppress = false;    // a widget may claim this drag below
            s_swipe_start_x  = s_mx;
            s_swipe_start_y  = s_my;
            dispMgr.requestShowNavArrows();
        }
        return;
    }
    if (s_swipe_active && !s_swipe_done) {
        const int dx  = s_swipe_last_x - s_swipe_start_x;
        const int dy  = s_swipe_last_y - s_swipe_start_y;
        const int adx = dx < 0 ? -dx : dx;
        const int ady = dy < 0 ? -dy : dy;
        // Same rules as the panel: far enough, more horizontal than vertical,
        // and not started in the left/right sixth where the nav arrows sit.
        const bool notOnButton = (s_swipe_start_x > UI_SCREEN_W / 6) &&
                                 (s_swipe_start_x < UI_SCREEN_W - UI_SCREEN_W / 6);
        if (adx >= UI_SWIPE_THRESHOLD && adx > ady && notOnButton && !s_swipe_suppress) {
            s_swipe_done = true;
            if (dx < 0) dispMgr.nextScreen();   // swipe left  → next
            else        dispMgr.prevScreen();   // swipe right → prev
        }
    }
    s_swipe_active = false;
}

void sim_mouse_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    // Replay one queued sample per read so LVGL cannot miss a press/release pair
    // — or the travel in between — that arrived within a single frame.
    if (!sim_q_empty()) {
        const SimEvt e = s_q[s_q_tail];
        s_q_tail = (s_q_tail + 1) % (int)(sizeof(s_q) / sizeof(s_q[0]));
        s_mx = e.x;
        s_my = e.y;
        if (!e.motion) s_pressed = e.pressed;
    }
    if (s_pressed) Entropy::feed((uint16_t)s_mx, (uint16_t)s_my);  // parity with the device
    sim_track_swipe(s_pressed);

    data->point.x = (lv_coord_t)s_mx;
    data->point.y = (lv_coord_t)s_my;
    data->state   = s_pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;

    // Ask LVGL to sample again immediately while transitions are still queued,
    // so a click does not stretch across several slow rendered frames.
    data->continue_reading = !sim_q_empty();
}

void sim_hal_handle_event(SDL_Event *ev) {
    if (!ev) return;
    if (ev->type == SDL_MOUSEMOTION) {
        sim_q_push(ev->motion.x, ev->motion.y, s_q_lastPressed, true);
    } else if (ev->type == SDL_MOUSEBUTTONDOWN &&
               ev->button.button == SDL_BUTTON_LEFT) {
        sim_q_push(ev->button.x, ev->button.y, true, false);
    } else if (ev->type == SDL_MOUSEBUTTONUP &&
               ev->button.button == SDL_BUTTON_LEFT) {
        sim_q_push(ev->button.x, ev->button.y, false, false);
    }
}
