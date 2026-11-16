#pragma once

#include "lvgl.h"

LV_FONT_DECLARE(lv_font_catvision_16);

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *catvision_label(lv_obj_t *parent, const char *text, int x, int y, int size, uint32_t color);
lv_obj_t *catvision_button(lv_obj_t *parent, const char *text, int x, int y, int width, lv_event_cb_t callback, void *user_data);
lv_obj_t *catvision_panel(lv_obj_t *parent, int x, int y, int width, int height, uint32_t color);

#ifdef __cplusplus
}
#endif
