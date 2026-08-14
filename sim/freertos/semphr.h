#pragma once
#include "FreeRTOS.h"
#ifndef SEMAPHORE_STUBS_DEFINED
#define SEMAPHORE_STUBS_DEFINED
// Real mutex in the simulator (the device uses a FreeRTOS mutex). A no-op here
// let the demo thread and the display loop race on the DataModel → intermittent
// crashes. recursive_mutex is defensive against any nested data.lock() path.
#include <mutex>
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return (SemaphoreHandle_t) new std::recursive_mutex(); }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t) {
    if (h) ((std::recursive_mutex *)h)->lock();
    return pdTRUE;
}
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t h) {
    if (h) ((std::recursive_mutex *)h)->unlock();
    return pdTRUE;
}
// Needed by DataModel::clearValues(), which creates a throwaway instance and
// must dispose of the semaphore that came with it.
inline void vSemaphoreDelete(SemaphoreHandle_t h) {
    delete (std::recursive_mutex *)h;
}
#endif
