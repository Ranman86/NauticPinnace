// esp_timer.h stub for PC simulator
#pragma once
#include <SDL2/SDL.h>
inline int64_t esp_timer_get_time() { return (int64_t)SDL_GetTicks() * 1000LL; }
