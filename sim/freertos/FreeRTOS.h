// FreeRTOS.h stub for PC simulator
#pragma once
#include <cstdint>

typedef void*    TaskHandle_t;
typedef void*    SemaphoreHandle_t;
typedef uint32_t TickType_t;
typedef long     BaseType_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMAX_DELAY     ((TickType_t)0xFFFFFFFF)

// vTaskDelay defined in arduino_stubs.h via SDL_Delay – avoid redefinition
#ifndef VTASKDELAY_DEFINED
#define VTASKDELAY_DEFINED
#include <SDL2/SDL.h>
inline void vTaskDelay(TickType_t ticks) { SDL_Delay((uint32_t)ticks); }
#endif

inline TaskHandle_t xTaskGetCurrentTaskHandle() { return nullptr; }

// xTaskCreatePinnedToCore stub
inline int xTaskCreatePinnedToCore(void(*f)(void*), const char*, uint32_t,
                                    void *arg, int, TaskHandle_t*, int) {
    // In simulator: just call the task function once (or ignore)
    return pdTRUE;
}
