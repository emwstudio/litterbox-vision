#include <stdlib.h>
#include "catvision_display.h"
#include "catvision_direct.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static esp_err_t present_owned(catvision_image_t *image, uint8_t *pixels, size_t size, int width, int height, uint32_t source_crc, bool byte_swap)
{
    if (!image || !image->object || !pixels || width <= 0 || height <= 0 || size != (size_t)width * height * 2) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *native = pixels;
    if (byte_swap) {
        if (image->lvgl_size != size) {
            uint8_t *resized = heap_caps_realloc(image->lvgl_pixels, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!resized) {
                return ESP_ERR_NO_MEM;
            }
            image->lvgl_pixels = resized;
            image->lvgl_size = size;
        }
        native = image->lvgl_pixels;
    }
    int64_t started = esp_timer_get_time();
    catvision_display_lock();
    lv_draw_wait_for_finish();
    int64_t waited = esp_timer_get_time();
    if (byte_swap) {
        for (size_t index = 0; index < size; index++) {
            native[index] = pixels[index ^ 1];
        }
    }
    int64_t converted = esp_timer_get_time();
    image->pixels = pixels;
    image->delta_base = pixels;
    image->source_crc = source_crc;
    image->descriptor.data = native;
    image->descriptor.data_size = size;
    image->descriptor.header.w = width;
    image->descriptor.header.h = height;
    image->descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    image->descriptor.header.stride = width * 2;
    catvision_direct_profile_frame(height);
    lv_image_set_src(image->object, &image->descriptor);
    lv_obj_clear_flag(image->object, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(image->object);
    catvision_display_unlock();
    static uint32_t profile_samples;
    if (profile_samples++ % 30 == 0) {
        ESP_LOGI("profile", "lvgl_wait_us=%lld swap_us=%lld submit_us=%lld",
                 waited - started, converted - waited, esp_timer_get_time() - converted);
    }
    return ESP_OK;
}

esp_err_t catvision_lvgl_present_owned_be(catvision_image_t *image, uint8_t *pixels, size_t size, int width, int height, uint32_t source_crc)
{
    return present_owned(image, pixels, size, width, height, source_crc, true);
}
