#include "catvision_display.h"
#include "catvision_direct.h"
#include "catvision_zlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"

static void rounder_event_cb(lv_event_t *event)
{
    lv_area_t *area = lv_event_get_param(event);
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

esp_err_t catvision_display_init(void)
{
    bsp_display_cfg_t board = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = {
#if BSP_LCD_H_RES == 466 && BSP_LCD_V_RES == 466
            .swap_xy = 0,
            .mirror_x = 1,
#else
            .swap_xy = 1,
            .mirror_x = 0,
#endif
            .mirror_y = 1,
        },
    };
    board.lv_adapter_cfg.task_priority = CONFIG_CATVISION_LVGL_TASK_PRIORITY;
    ESP_LOGI("display", "LVGL task priority: %d", (int)board.lv_adapter_cfg.task_priority);
    ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&board.lv_adapter_cfg), "display", "Adapter initialization failed");

    const bsp_display_config_t display_config = {
        .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * BSP_LCD_BITS_PER_PIXEL / 8,
    };
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(bsp_display_new(&display_config, &panel, &io), "display", "Panel initialization failed");

    ESP_RETURN_ON_ERROR(catvision_direct_prepare(), "display", "LCD callback ownership setup failed");
    esp_lv_adapter_display_config_t adapter_config = {
        .panel = panel,
        .panel_io = io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = board.rotation,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = 16,
            .use_psram = false,
            .enable_ppa_accel = false,
            .require_double_buffer = false,
        },
        .tear_avoid_mode = board.tear_avoid_mode,
    };
    lv_display_t *display = esp_lv_adapter_register_display(&adapter_config);
    if (!display) {
        return ESP_FAIL;
    }
    lv_display_add_event_cb(display, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    ESP_RETURN_ON_ERROR(catvision_direct_init(panel, io, display), "display", "Direct LCD initialization failed");

#if CONFIG_CATVISION_TOUCH_ENABLED
    esp_lcd_touch_handle_t touch = NULL;
    ESP_RETURN_ON_ERROR(bsp_touch_new(&board, &touch), "display", "Touch initialization failed");
    const esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(display, touch);
    if (!esp_lv_adapter_register_touch(&touch_config)) {
        return ESP_FAIL;
    }
#endif
    ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), "display", "Brightness initialization failed");
    ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), "display", "Adapter start failed");

    esp_err_t storage = bsp_spiffs_mount();
    if (storage != ESP_OK && storage != ESP_ERR_INVALID_STATE) {
        ESP_LOGW("display", "Image cache unavailable: %s", esp_err_to_name(storage));
    }
    return bsp_display_backlight_on();
}

void catvision_display_lock(void)
{
    bsp_display_lock(-1);
}

void catvision_display_unlock(void)
{
    bsp_display_unlock();
}

catvision_image_t *catvision_image_create(lv_obj_t *parent, int x, int y, int width, int height)
{
    catvision_image_t *image = calloc(1, sizeof(*image));
    if (!image) {
        return NULL;
    }
    image->object = lv_image_create(parent);
    lv_obj_set_pos(image->object, x, y);
    lv_obj_set_size(image->object, width, height);
    image->descriptor.header.w = width;
    image->descriptor.header.h = height;
    image->descriptor.header.cf = LV_COLOR_FORMAT_RAW;
    return image;
}

static esp_err_t update_image(catvision_image_t *image, const uint8_t *data, size_t size, int width, int height, lv_color_format_t format)
{
    if (!image || !data || size == 0 || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (format == LV_COLOR_FORMAT_RGB565 && size != (size_t)width * height * 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (format == LV_COLOR_FORMAT_RGB565) {
        return catvision_direct_update(image, data, size, width, height);
    }
    uint8_t *copy = malloc(size);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, data, size);
    catvision_display_lock();
    lv_draw_wait_for_finish();
    uint8_t *previous = image->pixels;
    image->pixels = copy;
    image->descriptor.data = copy;
    image->descriptor.data_size = size;
    image->descriptor.header.w = width;
    image->descriptor.header.h = height;
    image->descriptor.header.cf = format;
    image->descriptor.header.stride = format == LV_COLOR_FORMAT_RGB565 ? width * 2 : 0;
    lv_image_set_src(image->object, &image->descriptor);
    lv_obj_invalidate(image->object);
    catvision_display_unlock();
    free(previous);
    ESP_LOGI("display", "%s=%u bytes; DMA free=%u, largest=%u",
             format == LV_COLOR_FORMAT_RGB565 ? "RGB565" : "JPEG", size,
             heap_caps_get_free_size(MALLOC_CAP_DMA), heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    return ESP_OK;
}

esp_err_t catvision_image_update(catvision_image_t *image, const uint8_t *data, size_t size, int width, int height)
{
    return update_image(image, data, size, width, height, LV_COLOR_FORMAT_RAW);
}

esp_err_t catvision_image_update_rgb565(catvision_image_t *image, const uint8_t *data, size_t size, int width, int height)
{
    return update_image(image, data, size, width, height, LV_COLOR_FORMAT_RGB565);
}

esp_err_t catvision_image_update_zlib(catvision_image_t *image, const uint8_t *data, size_t size, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t decoded_size = (size_t)width * height * 2;
    uint8_t *decoded = malloc(decoded_size);
    if (!decoded) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = catvision_zlib_decompress(data, size, decoded, decoded_size);
    if (result == ESP_OK) {
        result = catvision_image_update_rgb565(image, decoded, decoded_size, width, height);
    }
    free(decoded);
    return result;
}

esp_err_t catvision_image_update_zlib_delta(catvision_image_t *image, const uint8_t *data, size_t size, bool delta, uint32_t expected_crc, int width, int height)
{
    return catvision_direct_update_delta(image, data, size, delta, expected_crc, width, height);
}

esp_err_t catvision_image_update_zlib_delta_native(catvision_image_t *image, const uint8_t *data, size_t size, bool delta, uint32_t expected_crc, int width, int height)
{
    return catvision_direct_update_delta_native(image, data, size, delta, expected_crc, width, height);
}

uint32_t catvision_image_crc(const catvision_image_t *image)
{
    return catvision_direct_crc(image);
}

void catvision_image_clear(catvision_image_t *image)
{
    if (!image) {
        return;
    }
    if (image->descriptor.header.cf == LV_COLOR_FORMAT_RGB565 && catvision_direct_wait_idle() != ESP_OK) {
        return;
    }
    catvision_display_lock();
    lv_draw_wait_for_finish();
    lv_obj_delete(image->object);
    catvision_display_unlock();
    if (image->delta_base && image->delta_base != image->pixels) {
        free(image->delta_base);
    }
    free(image->pixels);
    free(image->lvgl_pixels);
    free(image);
}

static uint32_t cache_hash(const char *url)
{
    uint32_t hash = 2166136261;
    while (*url) {
        hash ^= (uint8_t)*url++;
        hash *= 16777619;
    }
    return hash;
}

esp_err_t catvision_cache_store(const char *url, const uint8_t *data, size_t size)
{
    char path[64];
    snprintf(path, sizeof(path), "/spiffs/%08lx.img", (unsigned long)cache_hash(url));
    FILE *file = fopen(path, "wb");
    if (!file) {
        return ESP_FAIL;
    }
    size_t written = fwrite(data, 1, size, file);
    int result = fclose(file);
    return written == size && result == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t catvision_cache_load(const char *url, uint8_t **data, size_t *size)
{
    char path[64];
    snprintf(path, sizeof(path), "/spiffs/%08lx.img", (unsigned long)cache_hash(url));
    FILE *file = fopen(path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) {
        fclose(file);
        return ESP_FAIL;
    }
    uint8_t *buffer = malloc(length);
    if (!buffer) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    if (fread(buffer, 1, length, file) != (size_t)length) {
        free(buffer);
        fclose(file);
        return ESP_FAIL;
    }
    fclose(file);
    *data = buffer;
    *size = length;
    return ESP_OK;
}
