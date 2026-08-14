#pragma once
#include <lvgl.h>
#include "../nmea/DataModel.h"
#include "Theme.h"

// ============================================================
// BaseScreen – interface every display screen must implement.
// ============================================================
class BaseScreen {
public:
    virtual ~BaseScreen() = default;

    // Called once when the screen is first created.
    // Build all LVGL objects here; parent = lv_scr_act() equivalent.
    virtual void create(lv_obj_t *parent) = 0;

    // Called when this screen becomes active.
    virtual void onShow() {}

    // Called when this screen is hidden.
    virtual void onHide() {}

    // Called by the live theme rebuild AFTER the container (and thus all child
    // objects) has been deleted, BEFORE create() runs again. Subclasses that
    // cache child object pointers and lv_obj_del() them on rebuild MUST null
    // those pointers here (they are dangling after the container delete).
    virtual void resetForRebuild() {}

    // Called from the LVGL task ~once per second to refresh displayed values.
    virtual void update() = 0;

    // Human-readable name shown in the nav-bar title
    virtual const char *title() const = 0;

    // The LVGL container object (set inside create())
    lv_obj_t *container = nullptr;

protected:
    // Utility: format a float value with a fallback string for NAN
    static void fmtVal(char *buf, size_t sz, float val, int dec, const char *na = "--") {
        if (isnan(val)) snprintf(buf, sz, "%s", na);
        else {
            char fmt[16];
            snprintf(fmt, sizeof(fmt), "%%.%df", dec);
            snprintf(buf, sz, fmt, val);
        }
    }

};
