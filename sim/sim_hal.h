#pragma once
#include "sdl2_wrapper.h"  // SDL2 with compile-time assertion suppressed
#include <lvgl.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialise SDL2 window + LVGL port
bool sim_hal_init(int width, int height, const char *title);
void sim_hal_deinit(void);

// Call every frame to update LVGL tick counter
void sim_hal_tick(void);

// Save the current rendered frame to a BMP file (for headless inspection).
void sim_hal_screenshot(const char *path);

// LVGL driver callbacks
void sim_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p);
void sim_mouse_cb(lv_indev_drv_t *drv, lv_indev_data_t *data);

// Pass SDL events for mouse handling
struct SDL_Event;
void sim_hal_handle_event(struct SDL_Event *ev);

#ifdef __cplusplus
}
#endif
