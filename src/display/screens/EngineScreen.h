#pragma once
#include "../BaseScreen.h"
#include "../../i18n/I18n.h"

class EngineScreen : public BaseScreen {
public:
    const char *title() const override { return T(STR_SCREEN_ENGINE); }
    void create(lv_obj_t *parent) override;
    void onShow() override;   // rebuild bottom fields if config changed
    void update() override;
    void resetForRebuild() override {   // child objects already freed; null ptrs
        for (int i = 0; i < MAX_FIELDS; i++) _fields[i] = {};
        _fieldCount = 0; _arc = _lblRpm = _status = nullptr;
    }

private:
    static constexpr int MAX_FIELDS = 6;
    lv_obj_t *_arc       = nullptr;   // RPM gauge arc
    lv_obj_t *_lblRpm    = nullptr;
    lv_obj_t *_status    = nullptr;

    // Configurable bottom fields (count + data point from appConfig.cfg.engineFields)
    struct Field { lv_obj_t *card=nullptr, *val=nullptr; };
    Field _fields[MAX_FIELDS];
    int   _fieldCount = 0;
    lv_obj_t *_fieldRow = nullptr;   // parent container, retained for rebuilds
    void buildFields();
};
