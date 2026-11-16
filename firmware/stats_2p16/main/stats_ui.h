#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "catvision_display.h"
#include "lvgl.h"

esp_err_t stats_ui_init(lv_obj_t *parent, lv_obj_t *status_label);
void stats_ui_load_defaults(void);
esp_err_t stats_ui_update(const char *json);
esp_err_t stats_ui_update_frame(const uint8_t *data, size_t size);
esp_err_t stats_ui_update_frame_zlib(const uint8_t *data, size_t size);
esp_err_t stats_ui_update_frame_delta(const uint8_t *data, size_t size, bool delta, uint32_t expected_crc);
uint32_t stats_ui_frame_crc(void);
catvision_image_t *stats_ui_live_image(void);
