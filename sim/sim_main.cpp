// ============================================================
// sim_main.cpp  –  NauticPinnace PC Simulator entry point
//
// Runs the same DisplayManager + screens on Windows/Linux via SDL2.
// Demo data animates automatically (no ESP32 needed).
//
// Build:  pio run -e simulator
// Run:    .pio/build/simulator/program.exe
// ============================================================

#include "sim_hal.h"
#include "arduino_stubs.h"

// Pull in the app headers
#include "../src/display/DisplayManager.h"
#include "../src/display/ConfigOverlay.h"
#include "../src/display/LicenseOverlay.h"
#include "../src/display/LanguageOverlay.h"
#include "../src/display/BootScreen.h"
#include "../src/display/Theme.h"
#include "../src/display/UiConfig.h"
#include "../src/nmea/DataModel.h"
#include "../src/nmea/DemoData.h"
#include "../src/config/Config.h"
#include "../src/Entropy.h"

// ── Global instances (normally defined by main.cpp on ESP32) ─────────────────
// appConfig is defined in Config.cpp – do NOT redefine here
// data is defined here since main.cpp is excluded from the simulator build
DataModel  data;

// ── getTickFps (simulator: counts presented SDL frames) ───────────────────────
// g_sim_frames is incremented in sim_flush_cb() when a full frame is presented.
extern uint32_t g_sim_frames;
uint32_t getTickFps(bool reset) {
    uint32_t v = g_sim_frames;
    if (reset) g_sim_frames = 0;
    return v;
}
#include "../src/config/Config.h"
#include "../src/PsramArena.h"
#include "../src/PolarTable.h"

#include <lvgl.h>
#include <SDL2/SDL.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>

// ── LVGL draw buffer (SRAM on PC) ────────────────────────────────────────────
static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t         s_buf1[UI_SCREEN_W * 20];
static lv_color_t         s_buf2[UI_SCREEN_W * 20];

// ── Load the REAL device config so the sim shows the device's colours ────────
// Without this the simulator ran on the code-default palette while the panel used
// the 56 roles stored in data/config.json — so the two never matched and every
// colour check in the sim was meaningless. The device does the same thing in
// Config::begin(): defaults first, file on top.
// The exe runs from .pio/build/simulator, hence the walk back up.
static bool sim_load_device_config(const char *explicitPath) {
    static const char *candidates[] = {
        "data/config.json",
        "../../../data/config.json",
        "../../data/config.json",
    };
    const char *tried[4];
    int nTried = 0;
    const char *paths[4];
    int nPaths = 0;
    if (explicitPath) paths[nPaths++] = explicitPath;
    for (const char *c : candidates) paths[nPaths++] = c;

    for (int i = 0; i < nPaths; i++) {
        FILE *f = fopen(paths[i], "rb");
        tried[nTried++] = paths[i];
        if (!f) continue;
        std::string json;
        char chunk[4096];
        size_t n;
        while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) json.append(chunk, n);
        fclose(f);
        // Strip a UTF-8 BOM: ArduinoJson rejects it.
        if (json.size() >= 3 && (unsigned char)json[0] == 0xEF &&
            (unsigned char)json[1] == 0xBB && (unsigned char)json[2] == 0xBF)
            json.erase(0, 3);
        if (appConfig.fromJson(String(json.c_str()))) {
            printf("[sim] config loaded from %s  (theme='%s', lang='%s')\n",
                   paths[i], appConfig.cfg.themeActive, appConfig.cfg.lang);
            return true;
        }
        printf("[sim] %s found but could NOT be parsed — using defaults\n", paths[i]);
        return false;
    }
    printf("[sim] no data/config.json found (tried %d paths) — using code defaults\n", nTried);
    return false;
}

// ── Demo tick thread ──────────────────────────────────────────────────────────
static std::atomic<bool> s_running{true};

static std::atomic<bool> s_demo_enabled{false};

static void demo_thread_fn() {
    while (s_running) {
        if (s_demo_enabled) demoData.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

// ── main ──────────────────────────────────────────────────────────────────────
int main(int argc, char *argv[]) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: keep last marker on crash

    // --selftest  : walk the first-run sequence with fast clicks and a swipe,
    //               assert each step lands, exit 0/1. Regression guard for the
    //               lost-click bug (see sim_hal.cpp).
    // --firstrun  : show the first-run sequence (language picker -> licences).
    //               Off by default: the PC sim has no LittleFS and so cannot
    //               remember the consent, which would block the UI every start.
    // --de / --en : force the start language. Without either, the sim uses the
    //               same default as a fresh device (cfg.lang, i.e. English).
    bool selfTest = false, firstRun = false, openConfig = false;
    const char *forceLang = nullptr, *forceTheme = nullptr, *cfgPath = nullptr;
    int  startScreen = -1;      // --screen N : open that screen id straight away
    bool noDemo      = false;   // --nodemo   : no demo data at all (= no GPS fix)
    bool noPerf      = false;   // --noperf   : hide the fps/CPU overlay (for docs)
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--selftest") == 0) selfTest = true;
        if (strcmp(argv[i], "--firstrun") == 0) firstRun = true;
        if (strcmp(argv[i], "--config") == 0)   openConfig = true;
        if (strcmp(argv[i], "--light") == 0)    forceTheme = "light";
        if (strcmp(argv[i], "--dark") == 0)     forceTheme = "dark";
        if (strcmp(argv[i], "--night") == 0)    forceTheme = "night";
        if (strcmp(argv[i], "--cfg") == 0 && i + 1 < argc) cfgPath = argv[++i];
        if (strcmp(argv[i], "--screen") == 0 && i + 1 < argc) startScreen = atoi(argv[++i]);
        if (strcmp(argv[i], "--nodemo") == 0)  noDemo = true;
        if (strcmp(argv[i], "--noperf") == 0)  noPerf = true;
        if (strcmp(argv[i], "--en") == 0)       forceLang = "en";
        if (strcmp(argv[i], "--de") == 0)       forceLang = "de";
    }
    if (selfTest) firstRun = true;   // the picker is the first click target
    printf("NauticPinnace Simulator\n");
    printf("Resolution: %d x %d\n", UI_SCREEN_W, UI_SCREEN_H);
    printf("Close window or press ESC to quit.\n\n");

    // Init SDL2 + LVGL display/input
    if (!sim_hal_init(UI_SCREEN_W, UI_SCREEN_H, "NauticPinnace Simulator")) {
        fprintf(stderr, "SDL2 init failed!\n");
        return 1;
    }

    // Init LVGL
    lv_init();

    // Register display driver
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2,
                          UI_SCREEN_W * 20);
    disp_drv.draw_buf   = &s_draw_buf;
    disp_drv.flush_cb   = sim_flush_cb;
    disp_drv.hor_res    = UI_SCREEN_W;
    disp_drv.ver_res    = UI_SCREEN_H;
    lv_disp_drv_register(&disp_drv);

    // Register touch/mouse input driver
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = sim_mouse_cb;
    lv_indev_drv_register(&indev_drv);

    // Palette defaults FIRST, then the device config on top — exactly the order
    // Config::begin() uses on the panel, so the sim ends up with the same colours.
    fillLightTheme(appConfig.cfg.themeLight);
    fillNightTheme(appConfig.cfg.themeNight);
    const bool cfgLoaded = sim_load_device_config(cfgPath);
    if (!cfgLoaded) {
        // No config to mirror: fall back to the light theme, which is what the
        // simulator has always started in.
        strncpy(appConfig.cfg.themeActive, "light", sizeof(appConfig.cfg.themeActive) - 1);
    }
    if (forceTheme)
        strncpy(appConfig.cfg.themeActive, forceTheme, sizeof(appConfig.cfg.themeActive) - 1);
    // --firstrun/--selftest must ALWAYS show the first-run sequence, a normal
    // sim start NEVER: data/config.json is genuine factory state
    // (license_ok=false), so without setting this here every start would show
    // the language picker before the screens.
    appConfig.cfg.licenseAccepted = !firstRun;

    // Ensure a hotspot password just like on the device (the sim does not run
    // through Config::begin()) — otherwise the settings overlay showed the
    // placeholder instead of a password in the QR code.
    if (!appConfig.cfg.apPass[0])
        Entropy::generateApPassword(appConfig.cfg.apPass, sizeof(appConfig.cfg.apPass));

    i18nSetLang(i18nLangFromCode(forceLang ? forceLang : appConfig.cfg.lang));
    appConfig.cfg.demoMode       = !noDemo;
    // The sim has no radio; WiFi is "on" anyway so that the WiFi section of the
    // settings is shown in full (status + address instead of "WLAN aus").
    appConfig.cfg.wifiEnabled    = true;
    // On by default here (fps/CPU are useful while developing), but --noperf
    // turns it off — documentation screenshots must show what a device actually
    // ships with, and the factory config has it disabled.
    appConfig.cfg.showPerfOverlay = !noPerf;
    appConfig.cfg.waterlineLengthM = 9.0f;
    appConfig.cfg.allowSpinnaker   = true;

    // Populate the runtime theme/size/font globals (uiTheme/uiSz/uiFont). Without
    // this the FONT_*/CLR_* macros point at a NULL uiFont and the FIRST text
    // render segfaults — main.cpp calls this, the sim previously did not.
    applyThemeFromConfig();

    // Pre-allocate "PSRAM" arena on PC (normal heap)
    PsramArena::init(4 * 1024 * 1024);  // 4 MB

    // Allocate the polar table (else gPolar() dereferences a null polarPtr and
    // the Wind/Speed screens segfault). No load() — the PC has no polar.json, so
    // speedAt() returns NAN and the Wind screen uses its analytic fallback curve.
    PolarInit::init();

    // Build the display. Boat name comes from the config, exactly as on the
    // device (main.cpp), so the boot screen can be checked here.
    bootScreen.show(appConfig.cfg.bootName);
    dispMgr.begin();
    bootScreen.update("Simulator start...", 50);
    bootScreen.update("Bereit.", 100);

    // A quick lv_timer_handler flush to render boot screen
    for (int i = 0; i < 30; i++) {
        lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        sim_hal_tick();
    }
    // The boot screen is only up for ~1.5 s — capture it for visual checks.
    sim_hal_screenshot("boot_shot.bmp");

    dispMgr.activate();
    bootScreen.dismiss();
    // First-run sequence on the device: language picker, then licences. The PC
    // sim has no LittleFS, so the consent cannot be remembered and this would
    // reappear on every start — opt in with --firstrun when you want to test it.
    if (firstRun && !appConfig.cfg.licenseAccepted) languageOverlay.requestOpen();
    // --config: open the settings straight away (layout check without a mouse click).
    if (openConfig) dispMgr.requestOpenConfig();

    // Config overlay toggles for manual testing: C=open, X=close, K=keyboard.

    // --screen: jump straight to a screen id (layout checks without swiping).
    if (startScreen >= 0) dispMgr.showScreen(startScreen);

    // Start demo data in background thread. With --nodemo it never runs, so the
    // DataModel stays empty — that is how you reproduce "no GPS fix" on the PC.
    if (!noDemo) s_demo_enabled = true;
    std::thread demo_thread(demo_thread_fn);

    // Main loop
    uint32_t last_update = 0;
    while (s_running) {
        // Process SDL events
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { s_running = false; break; }
            if (ev.type == SDL_KEYDOWN) {
                if (ev.key.keysym.sym == SDLK_ESCAPE) { s_running = false; break; }
                if (ev.key.keysym.sym == SDLK_c) configOverlay.open();
                if (ev.key.keysym.sym == SDLK_x) configOverlay.close();
                if (ev.key.keysym.sym == SDLK_k) configOverlay.simShowKeyboard();
            }
            sim_hal_handle_event(&ev);
        }

        // LVGL tick
        sim_hal_tick();
        lv_timer_handler();

        // App update at ~10 fps
        uint32_t now = millis_stub();
        if (now - last_update >= 100) {
            last_update = now;
            dispMgr.update();   // refresh every 100 ms (matches the device loop)
        }

        // Periodic screenshot for the dev visual-feedback loop. Note: the default
        // (Wind) screen's canvas render is heavy in the PC software renderer
        // (~250 ms/frame), so 40 iterations ≈ a fresh sim_shot.bmp every ~10 s.
        static int shotFrame = 0;
        if (++shotFrame == 14 || shotFrame % 40 == 0) sim_hal_screenshot("sim_shot.bmp");

        // ---- --selftest: guard against the lost-click regression -------------
        // Injects a FAST click (DOWN and UP in the same SDL_PollEvent drain, the
        // case that used to be swallowed) on the licence overlay's button and
        // asserts the overlay actually closed.
        // Walks the whole first-run sequence with FAST clicks (DOWN and UP in the
        // same SDL_PollEvent drain — the case that used to be swallowed):
        //   language picker -> "English" -> licences -> "Understood" -> screens
        if (selfTest) {
            auto fastClick = [](int x, int y) {
                SDL_Event d{}, u{};
                d.type = SDL_MOUSEBUTTONDOWN; d.button.button = SDL_BUTTON_LEFT;
                d.button.x = x; d.button.y = y;
                u.type = SDL_MOUSEBUTTONUP;   u.button.button = SDL_BUTTON_LEFT;
                u.button.x = x; u.button.y = y;
                SDL_PushEvent(&d); SDL_PushEvent(&u);
            };
            // EVENT-DRIVEN, not time-driven: the debug sim renders the canvas
            // screens at ~1 fps — fixed millisecond gates (previously
            // 2.5 s per step) then falsely reported "click swallowed" even
            // though LVGL simply had not sampled the click yet.
            // Each step waits for the OBSERVABLE effect of the previous one and
            // only fails after a generous timeout.
            enum Step { ST_WAIT_LANG, ST_LANG_CLICKED, ST_LIC_CLICKED,
                        ST_SETTLE, ST_SWIPE_SENT };
            static Step     step      = ST_WAIT_LANG;
            static uint32_t stepSince = now;
            static int      settle    = 0, swipeFrom = -1;
            const uint32_t  STEP_TIMEOUT_MS = 30000;

            auto finish = [&](bool ok) -> int {
                sim_hal_screenshot(ok ? "selftest_pass.bmp" : "selftest_fail.bmp");
                printf("[selftest] %s\n", ok ? "ALL PASS" : "FAILURES ABOVE");
                s_running = false;
                if (demo_thread.joinable()) demo_thread.join();
                sim_hal_deinit();
                return ok ? 0 : 1;
            };
            const bool stepTimedOut = (now - stepSince) > STEP_TIMEOUT_MS;

            // Clicks are REPEATED as long as the target overlay is still open:
            // right after it is built LVGL has not done a layout pass yet,
            // the buttons' coordinates only settle after the next
            // refresh — an immediate click would hit nothing (verified:
            // lv_indev_search_obj returned NULL). Re-clicking every few
            // iterations still exercises the fast-click delivery.
            static int  clickIter = 0;
            switch (step) {
            case ST_WAIT_LANG:
                if (languageOverlay.isOpen()) {
                    sim_hal_screenshot("firstrun_1_language.bmp");
                    const bool pickDe = forceLang && forceLang[0] == 'd';
                    printf("[selftest] language picker is up, clicking '%s' ...\n",
                           pickDe ? "Deutsch" : "English");
                    step = ST_LANG_CLICKED; stepSince = now; clickIter = 0;
                } else if (stepTimedOut) {
                    printf("[selftest] FAIL: language picker did not open.\n");
                    return finish(false);
                }
                break;
            case ST_LANG_CLICKED:
                if (!languageOverlay.isOpen() && licenseOverlay.isOpen()) {
                    sim_hal_screenshot("firstrun_2_licence.bmp");
                    printf("[selftest] language=%s, licences followed; accepting ...\n",
                           i18nLangCode(i18nLang()));
                    step = ST_LIC_CLICKED; stepSince = now; clickIter = 0;
                } else if (stepTimedOut) {
                    printf("[selftest] FAIL: %s\n", languageOverlay.isOpen()
                           ? "language click was swallowed."
                           : "licences did not follow the language choice.");
                    return finish(false);
                } else if (languageOverlay.isOpen() && (clickIter++ % 3) == 0) {
                    // --de picks Deutsch (button 1, y 200..262), otherwise
                    // English (button 2, y 274..336). Both paths are worth
                    // testing: the German licence text is the one that can
                    // expose a missing glyph.
                    const bool pickDe = forceLang && forceLang[0] == 'd';
                    fastClick(240, pickDe ? 231 : 305);
                }
                break;
            case ST_LIC_CLICKED:
                if (!licenseOverlay.isOpen() && appConfig.cfg.licenseAccepted) {
                    printf("[selftest] first-run sequence completed — PASS\n");
                    step = ST_SETTLE; stepSince = now; settle = 0;
                } else if (stepTimedOut) {
                    printf("[selftest] FAIL: licence accept did not complete.\n");
                    return finish(false);
                } else if (licenseOverlay.isOpen() && (clickIter++ % 3) == 0) {
                    fastClick(240, 447);       // "Understood"
                }
                break;
            case ST_SETTLE:
                // cbAccept triggers requestThemeReload (screens are rebuilt)
                // — wait a few iterations so the swipe does not land in the
                // middle of the rebuild.
                if (++settle >= 3) {
                    swipeFrom = dispMgr.currentIndex();
                    printf("[selftest] swiping left from screen %d (%s) ...\n",
                           swipeFrom, dispMgr.currentTitle());
                    // Stage 2: a drag must reach LVGL as a swipe. The whole
                    // gesture is pushed in ONE go, which is what really
                    // happens: the main loop drains the entire SDL queue
                    // before LVGL ever samples, so a real mouse swipe arrives
                    // as DOWN + all MOTION + UP in a single pass. An earlier
                    // version spread the events over several frames and missed
                    // a bug that discarded all the travel.
                    // Start inside the middle two thirds: the panel ignores
                    // swipes beginning in the outer sixths (nav arrows).
                    SDL_Event d{};
                    d.type = SDL_MOUSEBUTTONDOWN; d.button.button = SDL_BUTTON_LEFT;
                    d.button.x = 380; d.button.y = 240;
                    SDL_PushEvent(&d);
                    for (int k = 1; k <= 6; k++) {
                        SDL_Event m{};
                        m.type = SDL_MOUSEMOTION;
                        m.motion.x = 380 - k * 45;
                        m.motion.y = 240;
                        SDL_PushEvent(&m);
                    }
                    SDL_Event u{};
                    u.type = SDL_MOUSEBUTTONUP; u.button.button = SDL_BUTTON_LEFT;
                    u.button.x = 110; u.button.y = 240;
                    SDL_PushEvent(&u);
                    step = ST_SWIPE_SENT; stepSince = now;
                }
                break;
            case ST_SWIPE_SENT:
                if (dispMgr.currentIndex() != swipeFrom) {
                    printf("[selftest] swipe %d -> %d (%s)  — PASS\n", swipeFrom,
                           dispMgr.currentIndex(), dispMgr.currentTitle());
                    return finish(true);
                } else if (stepTimedOut) {
                    printf("[selftest] swipe — FAIL, screen did not change\n");
                    return finish(false);
                }
                break;
            }
        }

// Layout survey aid (opt-in): #define CAPTURE_ALL_SCREENS at the top of this
// file to walk every screen, write scr_NN.bmp for each and quit. Used to measure
// dead space at the bottom of each screen. Give each screen a few seconds —
// Depth and Wind-Plot build their history over time and look empty before that.
#ifdef CAPTURE_ALL_SCREENS
        {
            static int capScr = 0;
            static uint32_t capT = 0;
            if (capT == 0) capT = now;
            if (now - capT > 4000) {                    // settle time per screen
                capT = now;
                char path[32];
                snprintf(path, sizeof(path), "scr_%02d.bmp", capScr);
                sim_hal_screenshot(path);
                if (++capScr >= 19) s_running = false;
                else dispMgr.showScreen(capScr);
            }
        }
#endif

        SDL_Delay(5);
    }

    s_running = false;
    demo_thread.join();
    sim_hal_deinit();
    printf("Simulator closed.\n");
    return 0;
}
