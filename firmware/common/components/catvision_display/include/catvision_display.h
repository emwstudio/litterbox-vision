#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_obj_t *object;
    lv_image_dsc_t descriptor;
    uint8_t *pixels;
    uint8_t *lvgl_pixels;
    size_t lvgl_size;
    uint8_t *delta_base;
    uint32_t source_crc;
} catvision_image_t;

esp_err_t catvision_display_init(void);
void catvision_display_lock(void);
void catvision_display_unlock(void);
catvision_image_t *catvision_image_create(lv_obj_t *parent, int x, int y, int width, int height);
esp_err_t catvision_image_update(catvision_image_t *image, const uint8_t *data, size_t size, int width, int height);
esp_err_t catvision_image_update_rgb565(catvision_image_t *image, const uint8_t *data, size_t size, int width, int height);
esp_err_t catvision_image_update_zlib(catvision_image_t *image, const uint8_t *data, size_t size, int width, int height);
esp_err_t catvision_image_update_zlib_delta(catvision_image_t *image, const uint8_t *data, size_t size, bool delta, uint32_t expected_crc, int width, int height);
esp_err_t catvision_image_update_zlib_delta_native(catvision_image_t *image, const uint8_t *data, size_t size, bool delta, uint32_t expected_crc, int width, int height);
uint32_t catvision_image_crc(const catvision_image_t *image);
esp_err_t catvision_lvgl_present_owned_be(catvision_image_t *image, uint8_t *pixels, size_t size, int width, int height, uint32_t source_crc);
void catvision_image_clear(catvision_image_t *image);
esp_err_t catvision_cache_store(const char *url, const uint8_t *data, size_t size);
esp_err_t catvision_cache_load(const char *url, uint8_t **data, size_t *size);

#ifdef __cplusplus
}
#endif
