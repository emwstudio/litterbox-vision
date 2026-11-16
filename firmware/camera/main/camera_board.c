#include "camera_board.h"

#include <string.h>
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "img_converters.h"

#define BUFFER_COUNT 2
#define JPEG_QUALITY 80

static const char *TAG = "camera_board";

static uint8_t *copy_bytes(const uint8_t *source, size_t size)
{
    uint8_t *copy = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        copy = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    if (copy) {
        memcpy(copy, source, size);
    }
    return copy;
}

static void capture_task(void *argument)
{
    camera_board_t *camera = argument;
    TickType_t frame_period = pdMS_TO_TICKS(1000 / camera->fps);
    if (frame_period == 0) {
        frame_period = 1;
    }
    TickType_t next_frame = xTaskGetTickCount();
    uint32_t window_frames = 0;
    int64_t window_started = esp_timer_get_time();
    while (!camera->capture_stop) {
        uint8_t *jpeg = NULL;
        size_t size = 0;
        esp_err_t result = camera_board_capture(camera, &jpeg, &size);
        xSemaphoreTake(camera->frame_lock, portMAX_DELAY);
        if (result != ESP_OK || size == 0 || size > CAMERA_JPEG_CAP) {
            camera->capture_errors++;
            heap_caps_free(jpeg);
        } else {
            uint8_t *previous = camera->latest;
            int64_t now = esp_timer_get_time();
            camera->latest = jpeg;
            camera->latest_size = size;
            camera->latest_sequence++;
            camera->captured_frames++;
            camera->last_frame_us = now;
            window_frames++;
            int64_t elapsed = now - window_started;
            if (elapsed >= 2000000) {
                camera->measured_fps = (float)window_frames * 1000000.0f / (float)elapsed;
                window_frames = 0;
                window_started = now;
            }
            xSemaphoreGive(camera->frame_lock);
            heap_caps_free(previous);
            vTaskDelayUntil(&next_frame, frame_period);
            continue;
        }
        xSemaphoreGive(camera->frame_lock);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    camera->capture_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t camera_board_init(camera_board_t *camera, uint32_t width, uint32_t height, uint32_t fps)
{
    if (!camera || width != 800 || height != 600 || fps == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(camera, 0, sizeof(*camera));
    camera->lock = xSemaphoreCreateMutex();
    camera->frame_lock = xSemaphoreCreateMutex();
    if (!camera->lock || !camera->frame_lock) {
        camera_board_deinit(camera);
        return ESP_ERR_NO_MEM;
    }

    camera_config_t config = {
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = 38,
        .pin_sccb_sda = GPIO_NUM_NC,
        .pin_sccb_scl = GPIO_NUM_NC,
        .pin_d7 = 21,
        .pin_d6 = 39,
        .pin_d5 = 40,
        .pin_d4 = 42,
        .pin_d3 = 46,
        .pin_d2 = 48,
        .pin_d1 = 47,
        .pin_d0 = 45,
        .pin_vsync = 17,
        .pin_href = 18,
        .pin_pclk = 41,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_SVGA,
        .jpeg_quality = 12,
        .fb_count = BUFFER_COUNT,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
        .sccb_i2c_port = 0,
        .jpeg_buffer_size = 0,
    };
    esp_err_t result = esp_camera_init(&config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(result));
        camera_board_deinit(camera);
        return result;
    }
    camera->width = width;
    camera->height = height;
    camera->fps = fps;
    if (xTaskCreatePinnedToCore(capture_task, "camera_capture", 4096, camera, 2, &camera->capture_task, 1) != pdPASS) {
        camera_board_deinit(camera);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "OV5640 ready: %lux%lu at %lu fps", camera->width, camera->height, camera->fps);
    return ESP_OK;
}

esp_err_t camera_board_capture(camera_board_t *camera, uint8_t **jpeg, size_t *jpeg_size)
{
    if (!camera || !jpeg || !jpeg_size) {
        return ESP_ERR_INVALID_ARG;
    }
    *jpeg = NULL;
    *jpeg_size = 0;
    if (xSemaphoreTake(camera->lock, pdMS_TO_TICKS(3000)) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t result = ESP_OK;
    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame) {
        result = ESP_FAIL;
        goto done;
    }
    if (frame->format == PIXFORMAT_JPEG) {
        *jpeg = copy_bytes(frame->buf, frame->len);
        if (!*jpeg) {
            result = ESP_ERR_NO_MEM;
        } else {
            *jpeg_size = frame->len;
        }
    } else if (!fmt2jpg(frame->buf, frame->len, frame->width, frame->height,
                        frame->format, JPEG_QUALITY, jpeg, jpeg_size)) {
        result = ESP_FAIL;
    }
    esp_camera_fb_return(frame);

done:
    xSemaphoreGive(camera->lock);
    return result;
}

esp_err_t camera_board_copy_latest(camera_board_t *camera, uint8_t *destination, size_t capacity, uint32_t *sequence, size_t *size)
{
    if (!camera || !camera->frame_lock || !destination || !sequence || !size) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(camera->frame_lock, portMAX_DELAY) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t result = ESP_OK;
    if (!camera->latest || camera->latest_size == 0 || *sequence == camera->latest_sequence) {
        result = ESP_ERR_NOT_FOUND;
    } else if (camera->latest_size > capacity) {
        result = ESP_ERR_INVALID_SIZE;
    } else {
        memcpy(destination, camera->latest, camera->latest_size);
        *size = camera->latest_size;
        *sequence = camera->latest_sequence;
    }
    xSemaphoreGive(camera->frame_lock);
    return result;
}

void camera_board_deinit(camera_board_t *camera)
{
    if (!camera) {
        return;
    }
    if (camera->capture_task) {
        camera->capture_stop = true;
        for (int waited = 0; waited < 300 && camera->capture_task; waited++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (camera->capture_task) {
            ESP_LOGW(TAG, "Capture task did not stop; board resources retained");
            return;
        }
    }
    esp_camera_deinit();
    if (camera->lock) {
        vSemaphoreDelete(camera->lock);
    }
    if (camera->frame_lock) {
        vSemaphoreDelete(camera->frame_lock);
    }
    heap_caps_free(camera->latest);
    memset(camera, 0, sizeof(*camera));
}
