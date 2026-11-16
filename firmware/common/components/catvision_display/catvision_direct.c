#include "catvision_direct.h"
#include "catvision_crc.h"
#include "catvision_half.h"
#include "catvision_present.h"
#include "catvision_zlib.h"

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#ifdef ESP_PLATFORM
#include "freertos/portmacro.h"
#endif
#include "freertos/semphr.h"

#define DIRECT_BUFFER_LINES 32
#define DIRECT_BUFFER_COUNT 2

typedef enum {
    TRANSFER_NONE,
    TRANSFER_LVGL,
    TRANSFER_DIRECT,
} transfer_owner_t;

static esp_lcd_panel_handle_t direct_panel;
static esp_lcd_panel_io_handle_t direct_io;
static lv_display_t *direct_display;
static lv_obj_t *direct_overlay;
static uint8_t *direct_buffers[DIRECT_BUFFER_COUNT];
static size_t direct_buffer_index;
static SemaphoreHandle_t direct_slots;
static SemaphoreHandle_t direct_frame_idle;
static volatile transfer_owner_t current_owner;
static volatile size_t direct_chunks_left;
static volatile bool direct_submission_done;
static uint32_t direct_present_count;
static uint32_t direct_callback_count;
static int lvgl_profile_frame_lines;
static int lvgl_profile_lines;
static int lvgl_profile_chunk_lines;
static int64_t lvgl_profile_started;
static int64_t lvgl_profile_completed_at;
static uint32_t lvgl_profile_frames;
#ifdef ESP_PLATFORM
static portMUX_TYPE direct_lock = portMUX_INITIALIZER_UNLOCKED;
#endif

static esp_err_t lvgl_draw_bitmap(lv_display_t *display, esp_lcd_panel_handle_t panel,
                                  int x_start, int y_start, int x_end, int y_end,
                                  const void *color_map, void *user_context)
{
    static bool logged_dma_source;
    if (!logged_dma_source) {
        ESP_LOGI("display", "LVGL buffer dma=%d", esp_ptr_dma_capable(color_map));
        logged_dma_source = true;
    }
    if (current_owner != TRANSFER_NONE) {
        return ESP_ERR_NOT_ALLOWED;
    }
    current_owner = TRANSFER_LVGL;
    if (y_start == 0) {
        lvgl_profile_started = esp_timer_get_time();
        lvgl_profile_lines = 0;
    }
    lvgl_profile_chunk_lines = y_end - y_start;
    esp_err_t result = esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, color_map);
    if (result != ESP_OK) {
        current_owner = TRANSFER_NONE;
        ESP_LOGE("display", "LVGL buffer=%p dma=%d bytes=%u free=%u largest=%u",
                 color_map, esp_ptr_dma_capable(color_map), (x_end - x_start) * (y_end - y_start) * 2,
                 heap_caps_get_free_size(MALLOC_CAP_DMA), heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    }
    return result;
}

static int64_t lvgl_profile_last_dma_us;
static int64_t lvgl_profile_last_interval_us;

static bool lcd_transfer_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *user_context)
{
    if (current_owner == TRANSFER_DIRECT) {
        direct_callback_count++;
        BaseType_t yield = pdFALSE;
        xSemaphoreGiveFromISR(direct_slots, &yield);
        if (direct_chunks_left > 0) {
            direct_chunks_left--;
        }
        if (direct_chunks_left == 0 && direct_submission_done) {
            current_owner = TRANSFER_NONE;
            xSemaphoreGiveFromISR(direct_frame_idle, &yield);
        }
        return yield == pdTRUE;
    }
    lvgl_profile_lines += lvgl_profile_chunk_lines;
    if (lvgl_profile_frame_lines > 0 && lvgl_profile_lines >= lvgl_profile_frame_lines) {
        lvgl_profile_frames++;
        int64_t completed = esp_timer_get_time();
        lvgl_profile_last_dma_us = completed - lvgl_profile_started;
        lvgl_profile_last_interval_us = completed - lvgl_profile_completed_at;
        lvgl_profile_completed_at = completed;
        lvgl_profile_lines = 0;
    }
    bool yield = esp_lv_adapter_display_notify_color_trans_done_from_isr(direct_display);
    current_owner = TRANSFER_NONE;
    return yield;
}

void catvision_direct_profile_frame(int height)
{
    if (height > 0) {
        lvgl_profile_frame_lines = height;
    }
    static uint32_t logged_frames;
    if (lvgl_profile_frames > logged_frames && lvgl_profile_frames % 16 == 0) {
        logged_frames = lvgl_profile_frames;
        ESP_LOGI("lcd", "completed_frames=%lu dma_us=%lld interval_us=%lld",
                 (unsigned long)lvgl_profile_frames, lvgl_profile_last_dma_us, lvgl_profile_last_interval_us);
    }
}

uint32_t catvision_direct_lvgl_completed_frames(void)
{
    return lvgl_profile_frames;
}

esp_err_t catvision_direct_prepare(void)
{
    return esp_lv_adapter_set_default_display_idf_callback_registration_enabled(false);
}

esp_err_t catvision_direct_init(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t io, lv_display_t *display)
{
    if (!panel || !io || !display || direct_frame_idle) {
        return ESP_ERR_INVALID_ARG;
    }
    direct_panel = panel;
    direct_io = io;
    direct_display = display;
    int width = lv_display_get_horizontal_resolution(display);
    if (width <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t buffer_size = (size_t)width * DIRECT_BUFFER_LINES * 2;
    for (int index = 0; index < DIRECT_BUFFER_COUNT; index++) {
        direct_buffers[index] = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    }
    direct_slots = xSemaphoreCreateCounting(DIRECT_BUFFER_COUNT, DIRECT_BUFFER_COUNT);
    direct_frame_idle = xSemaphoreCreateBinary();
    if (!direct_buffers[0] || !direct_buffers[1] || !direct_slots || !direct_frame_idle) {
        for (int index = 0; index < DIRECT_BUFFER_COUNT; index++) {
            free(direct_buffers[index]);
            direct_buffers[index] = NULL;
        }
        if (direct_slots) {
            vSemaphoreDelete(direct_slots);
            direct_slots = NULL;
        }
        if (direct_frame_idle) {
            vSemaphoreDelete(direct_frame_idle);
            direct_frame_idle = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(direct_frame_idle);
    const esp_lv_adapter_draw_bitmap_callbacks_t draw_callbacks = {
        .custom_draw_bitmap = lvgl_draw_bitmap,
    };
    esp_err_t result = esp_lv_adapter_set_draw_bitmap_callbacks(display, &draw_callbacks, NULL);
    if (result != ESP_OK) {
        return result;
    }
    const esp_lcd_panel_io_callbacks_t io_callbacks = {
        .on_color_trans_done = lcd_transfer_done,
    };
    result = esp_lcd_panel_io_register_event_callbacks(io, &io_callbacks, NULL);
    if (result == ESP_OK) {
        result = catvision_half_init();
    }
    if (result == ESP_OK) {
        result = catvision_present_init();
    }
    return result;
}

void catvision_direct_set_overlay(lv_obj_t *label)
{
    direct_overlay = label;
}

static esp_err_t wait_for_other_transfer(void)
{
    uint32_t started = lv_tick_get();
    while (current_owner != TRANSFER_NONE) {
        if (lv_tick_elaps(started) >= 1000) {
            ESP_LOGE("display", "wait_for_other_transfer timeout owner=%d", current_owner);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_OK;
}

esp_err_t catvision_direct_wait_idle(void)
{
    if (!direct_frame_idle) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t result = catvision_present_wait();
    if (result != ESP_OK) {
        return result;
    }
    if (xSemaphoreTake(direct_frame_idle, pdMS_TO_TICKS(1000)) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(direct_frame_idle);
    return ESP_OK;
}

static esp_err_t direct_present(catvision_image_t *image, const uint8_t *data, size_t size, uint8_t *retained, uint32_t source_crc, int width, int height, bool swap_bytes)
{
    if (!image || !data || width <= 0 || height <= 0 || size != (size_t)width * height * 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!direct_panel || !direct_io || !direct_display || !direct_slots || !direct_frame_idle) {
        return ESP_ERR_INVALID_STATE;
    }
    if (direct_overlay && !swap_bytes) {
        return catvision_lvgl_present_owned_be(image, retained, size, width, height, source_crc);
    }
    int64_t started = esp_timer_get_time();
    direct_present_count++;
    if (xSemaphoreTake(direct_frame_idle, pdMS_TO_TICKS(1000)) != pdPASS) {
        ESP_LOGE("display", "direct transfer timeout waiting for the previous frame");
        return ESP_ERR_TIMEOUT;
    }
    int64_t waited = esp_timer_get_time();
    catvision_display_lock();
    lv_draw_wait_for_finish();
    esp_err_t result = wait_for_other_transfer();
    if (result != ESP_OK) {
        catvision_display_unlock();
        xSemaphoreGive(direct_frame_idle);
        return result;
    }
    waited = esp_timer_get_time();
    if (current_owner != TRANSFER_NONE) {
        catvision_display_unlock();
        xSemaphoreGive(direct_frame_idle);
        return ESP_ERR_INVALID_STATE;
    }
    lv_obj_add_flag(image->object, LV_OBJ_FLAG_HIDDEN);
    int x = lv_obj_get_x(image->object);
    int y = lv_obj_get_y(image->object);
    direct_chunks_left = 0;
    direct_submission_done = false;
    result = ESP_OK;
    int64_t convert_us = 0;
    int64_t submit_us = 0;
    for (int row = 0; row < height; row += DIRECT_BUFFER_LINES) {
        int lines = height - row < DIRECT_BUFFER_LINES ? height - row : DIRECT_BUFFER_LINES;
        size_t chunk_size = (size_t)width * lines * 2;
        if (xSemaphoreTake(direct_slots, pdMS_TO_TICKS(1000)) != pdPASS) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        int64_t converting = esp_timer_get_time();
        uint8_t *target = direct_buffers[direct_buffer_index];
        direct_buffer_index = (direct_buffer_index + 1) % DIRECT_BUFFER_COUNT;
        memcpy(target, data + (size_t)row * width * 2, chunk_size);
        if (swap_bytes) {
            for (size_t index = 0; index < chunk_size; index += 2) {
                uint8_t low = target[index];
                target[index] = target[index + 1];
                target[index + 1] = low;
            }
        }
        int64_t converted = esp_timer_get_time();
        convert_us += converted - converting;
        portENTER_CRITICAL(&direct_lock);
        current_owner = TRANSFER_DIRECT;
        direct_chunks_left++;
        portEXIT_CRITICAL(&direct_lock);
        result = esp_lcd_panel_draw_bitmap(direct_panel, x, y + row, x + width, y + row + lines, target);
        submit_us += esp_timer_get_time() - converted;
        if (result != ESP_OK) {
            portENTER_CRITICAL(&direct_lock);
            direct_chunks_left--;
            portEXIT_CRITICAL(&direct_lock);
            xSemaphoreGive(direct_slots);
            ESP_LOGE("display", "direct transfer failed: %s; buffer=%p dma=%d bytes=%u free=%u largest=%u",
                     esp_err_to_name(result), target, esp_ptr_dma_capable(target), chunk_size,
                     heap_caps_get_free_size(MALLOC_CAP_DMA), heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
            break;
        }
    }
    int64_t submitted = esp_timer_get_time();
    direct_submission_done = true;
    if (direct_chunks_left > 0) {
        if (xSemaphoreTake(direct_frame_idle, pdMS_TO_TICKS(1000)) != pdPASS) {
            ESP_LOGE("display", "direct transfer timeout waiting for DMA");
            if (result == ESP_OK) {
                result = ESP_ERR_TIMEOUT;
            }
        } else {
            xSemaphoreGive(direct_frame_idle);
        }
    } else {
        current_owner = TRANSFER_NONE;
        xSemaphoreGive(direct_frame_idle);
    }
    int64_t transferred = esp_timer_get_time();
    if (result == ESP_OK) {
        uint8_t *previous = image->pixels;
        image->pixels = retained;
        image->source_crc = source_crc;
        if (previous && previous != retained && previous != image->delta_base) {
            free(previous);
        }
        if (!image->delta_base || image->delta_base == previous) {
            image->delta_base = retained;
        }
    }
    image->descriptor.data = NULL;
    image->descriptor.data_size = size;
    image->descriptor.header.w = width;
    image->descriptor.header.h = height;
    image->descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
    image->descriptor.header.stride = width * 2;
    if (direct_overlay) {
        lv_obj_invalidate(direct_overlay);
    }
    catvision_display_unlock();
    if (result == ESP_OK) {
        static uint32_t profile_samples;
        if (profile_samples++ % 30 == 0) {
            ESP_LOGI("profile", "wait_us=%lld lock_convert_submit_us=%lld convert_us=%lld submit_us=%lld dma_us=%lld",
                     (waited - started), (submitted - waited), convert_us, submit_us, (transferred - submitted));
        }
    }
    if (result != ESP_OK) {
        ESP_LOGE("display", "result=%d owner=%d chunks=%u submitted=%d presents=%lu callbacks=%lu",
                 result, current_owner, (unsigned int)direct_chunks_left, direct_submission_done,
                 (unsigned long)direct_present_count, (unsigned long)direct_callback_count);
    }
    return result;
}

esp_err_t catvision_direct_update(catvision_image_t *image, const uint8_t *data, size_t size, int width, int height)
{
    if (!image || !data || width <= 0 || height <= 0 || size != (size_t)width * height * 2) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *retained = malloc(size);
    if (!retained) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(retained, data, size);
    esp_err_t result = direct_present(image, data, size, retained, catvision_crc32(retained, size), width, height, true);
    if (result != ESP_OK) {
        free(retained);
    }
    return result;
}

esp_err_t catvision_direct_update_owned(catvision_image_t *image, uint8_t *data, size_t size, uint32_t expected_crc, int width, int height)
{
    return direct_present(image, data, size, data, expected_crc, width, height, true);
}

esp_err_t catvision_direct_update_owned_native(catvision_image_t *image, uint8_t *data, size_t size, uint32_t expected_crc, int width, int height)
{
    return direct_present(image, data, size, data, expected_crc, width, height, false);
}

uint32_t catvision_direct_crc(const catvision_image_t *image)
{
    return image && image->pixels ? image->source_crc : 0;
}

esp_err_t catvision_direct_decode_delta(catvision_image_t *image, const uint8_t *data, size_t size, bool delta, uint32_t expected_crc, int width, int height, uint8_t **decoded)
{
    if (!decoded) {
        return ESP_ERR_INVALID_ARG;
    }
    *decoded = NULL;
    if (!image || !data || size == 0 || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t raw_size = (size_t)width * height * 2;
    int64_t started = esp_timer_get_time();
    uint8_t *buffer = malloc(raw_size);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = catvision_zlib_decompress(data, size, buffer, raw_size);
    int64_t inflated = esp_timer_get_time();
    if (result == ESP_OK && delta) {
        if (!image->pixels) {
            result = ESP_ERR_INVALID_STATE;
        } else {
            uint32_t *decoded_words = (uint32_t *)buffer;
            const uint32_t *previous_words = (const uint32_t *)image->pixels;
            size_t word_count = raw_size / 4;
            for (size_t index = 0; index < word_count; index++) {
                decoded_words[index] ^= previous_words[index];
            }
            for (size_t index = word_count * 4; index < raw_size; index++) {
                buffer[index] ^= image->pixels[index];
            }
        }
    }
    int64_t restored = esp_timer_get_time();
    uint32_t decoded_crc = result == ESP_OK ? catvision_crc32(buffer, raw_size) : 0;
    if (result == ESP_OK && decoded_crc != expected_crc) {
        result = ESP_ERR_INVALID_RESPONSE;
    }
    int64_t validated = esp_timer_get_time();
    if (result == ESP_OK) {
        *decoded = buffer;
        ESP_LOGI("profile", "inflate_us=%lld xor_us=%lld verify_us=%lld",
                 (inflated - started), (restored - inflated), (validated - restored));
    } else {
        ESP_LOGW("display", "delta decode result=%d expected_crc=%lu decoded_crc=%lu current_crc=%lu inflate_us=%lld",
                 result, (unsigned long)expected_crc, (unsigned long)decoded_crc,
                 (unsigned long)catvision_direct_crc(image), (inflated - started));
        free(buffer);
    }
    return result;
}

static esp_err_t direct_update_delta(catvision_image_t *image, const uint8_t *data, size_t size, bool delta, uint32_t expected_crc, int width, int height, bool native)
{
    uint8_t *decoded = NULL;
    esp_err_t result = catvision_direct_decode_delta(image, data, size, delta, expected_crc, width, height, &decoded);
    if (result == ESP_OK) {
        size_t raw_size = (size_t)width * height * 2;
        result = native ? catvision_direct_update_owned_native(image, decoded, raw_size, expected_crc, width, height)
                        : catvision_direct_update_owned(image, decoded, raw_size, expected_crc, width, height);
    }
    if (result != ESP_OK) {
        free(decoded);
    }
    return result;
}

esp_err_t catvision_direct_update_delta(catvision_image_t *image, const uint8_t *data, size_t size, bool delta, uint32_t expected_crc, int width, int height)
{
    return direct_update_delta(image, data, size, delta, expected_crc, width, height, false);
}

esp_err_t catvision_direct_update_delta_native(catvision_image_t *image, const uint8_t *data, size_t size, bool delta, uint32_t expected_crc, int width, int height)
{
    return direct_update_delta(image, data, size, delta, expected_crc, width, height, true);
}

esp_err_t catvision_direct_decode_halves(catvision_image_t *image, const uint8_t *data, size_t size, uint32_t expected_crc, int width, int height, uint8_t **decoded)
{
    return catvision_half_decode(image, data, size, expected_crc, width, height, decoded);
}

esp_err_t catvision_direct_decode_parts(catvision_image_t *image, const uint8_t *data, size_t size, int parts, uint32_t expected_crc, int width, int height, uint8_t **decoded)
{
    return catvision_half_decode_parts(image, data, size, parts, expected_crc, width, height, decoded);
}
