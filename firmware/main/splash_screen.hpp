#pragma once

#include "lvgl.h"

class SplashScreen {
public:
    void show(const char *status = nullptr);
    void set_status(const char *status);
    void dismiss();
    void forget();
    lv_obj_t *screen() { return screen_; }

private:
    lv_obj_t *screen_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
};
