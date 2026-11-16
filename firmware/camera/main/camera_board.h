#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CAMERA_JPEG_CAP (256 * 1024)

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t frame_lock;
    uint8_t *latest;
    size_t latest_size;
    uint32_t latest_sequence;
    uint32_t captured_frames;
    uint32_t capture_errors;
    int64_t last_frame_us;
    float measured_fps;
    TaskHandle_t capture_task;
    volatile bool capture_stop;
} camera_board_t;

esp_err_t camera_board_init(camera_board_t *camera, uint32_t width, uint32_t height, uint32_t fps);
esp_err_t camera_board_capture(camera_board_t *camera, uint8_t **jpeg, size_t *jpeg_size);
esp_err_t camera_board_copy_latest(camera_board_t *camera, uint8_t *destination, size_t capacity, uint32_t *sequence, size_t *size);
void camera_board_deinit(camera_board_t *camera);
