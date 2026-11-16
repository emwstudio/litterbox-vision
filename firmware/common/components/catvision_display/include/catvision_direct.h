#pragma once

#include <stddef.h>
#include <stdint.h>
#include "catvision_display.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lv_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t catvision_direct_prepare(void);
esp_err_t catvision_direct_init(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io, lv_display_t *display);
esp_err_t catvision_direct_update(catvision_image_t *image, const uint8_t *data, size_t size, int width, int height);
esp_err_t catvision_direct_update_owned(catvision_image_t *image, uint8_t *data, size_t size, uint32_t expected_crc, int width, int height);
esp_err_t catvision_direct_update_owned_native(catvision_image_t *image, uint8_t *data, size_t size, uint32_t expected_crc, int width, int height);
esp_err_t catvision_direct_update_owned_native_async(catvision_image_t *image, uint8_t *data, size_t size, uint32_t expected_crc, int width, int height);
esp_err_t catvision_direct_commit_decoded(catvision_image_t *image, uint8_t *data);
esp_err_t catvision_direct_abort_pending(catvision_image_t *image);
esp_err_t catvision_direct_decode_delta(catvision_image_t *image, const uint8_t *data, size_t size, bool delta, uint32_t expected_crc, int width, int height, uint8_t **decoded);
esp_err_t catvision_direct_decode_halves(catvision_image_t *image, const uint8_t *data, size_t size, uint32_t expected_crc, int width, int height, uint8_t **decoded);
esp_err_t catvision_direct_decode_parts(catvision_image_t *image, const uint8_t *data, size_t size, int parts, uint32_t expected_crc, int width, int height, uint8_t **decoded);
esp_err_t catvision_direct_update_delta(catvision_image_t *image, const uint8_t *data, size_t size, bool delta, uint32_t expected_crc, int width, int height);
esp_err_t catvision_direct_update_delta_native(catvision_image_t *image, const uint8_t *data, size_t size, bool delta, uint32_t expected_crc, int width, int height);
uint32_t catvision_direct_crc(const catvision_image_t *image);
void catvision_direct_set_overlay(lv_obj_t *label);
void catvision_direct_profile_frame(int height);
uint32_t catvision_direct_lvgl_completed_frames(void);
esp_err_t catvision_direct_wait_idle(void);

#ifdef __cplusplus
}
#endif
