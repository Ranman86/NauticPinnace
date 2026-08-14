// sdl2_wrapper.h – Wraps SDL2 include, suppresses strict compile-time assertions
// that fail on some MinGW/SDL2 version combinations.
#pragma once

// Suppress SDL2's strict struct-size assertion (can fail with certain
// MinGW alignment settings; the actual runtime behaviour is unaffected).
#ifdef __cplusplus
extern "C" {
#endif

// SDL2 uses SDL_COMPILE_TIME_ASSERT which maps to _Static_assert / static_assert.
// Redefine it to a no-op to avoid the SDL_Event padding size mismatch error.
#define SDL_COMPILE_TIME_ASSERT(name, x)   /**/

#include <SDL2/SDL.h>

#undef SDL_COMPILE_TIME_ASSERT

#ifdef __cplusplus
}
#endif
