// simulator_defines.h
// Force-included via -include flag for ALL simulator compilation units.
// Neutralises ESP32-specific GCC section/placement attributes that
// MinGW/GCC on Windows does not support.

#ifndef SIMULATOR_DEFINES_H
#define SIMULATOR_DEFINES_H

// Neutralise ESP32 IRAM/DRAM placement attributes
#define LV_ATTRIBUTE_FAST_MEM
#define LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_LARGE_CONST
#define LV_ATTRIBUTE_LARGE_RAM_ARRAY
#define LV_ATTRIBUTE_EXTERN_DATA_SECTION
#define LV_ATTRIBUTE_TIMER_HANDLER
#define LV_ATTRIBUTE_FLUSH_READY
#define IRAM_ATTR
#define DRAM_ATTR
#define RODATA_ATTR

// Ensure SIMULATOR is defined (in case compiler is invoked without it)
#ifndef SIMULATOR
#define SIMULATOR 1
#endif

// The PC has plenty of RAM and its LVGL objects are larger than the device's;
// give the simulator a much bigger pool so all screens fit (device keeps 96 KB).
#define LV_MEM_SIZE   (320U * 1024U)

#endif // SIMULATOR_DEFINES_H
