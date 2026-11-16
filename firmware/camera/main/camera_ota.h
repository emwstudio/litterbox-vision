#pragma once

#include <stdint.h>
#include "camera_board.h"
#include "esp_err.h"
#include "sdkconfig.h"

#ifndef CONFIG_CATVISION_OTA_SELF_TEST_FAILURE
#define CONFIG_CATVISION_OTA_SELF_TEST_FAILURE 0
#endif

typedef enum {
    CAMERA_OTA_IDLE,
    CAMERA_OTA_DOWNLOADING,
    CAMERA_OTA_RESTARTING,
    CAMERA_OTA_ERROR,
} camera_ota_state_t;

typedef struct {
    camera_ota_state_t state;
    int32_t bytes;
    esp_err_t error;
} camera_ota_status_t;

esp_err_t camera_ota_start(const char *url);
void camera_ota_status(camera_ota_status_t *status);
const char *camera_ota_state_name(camera_ota_state_t state);
esp_err_t camera_ota_confirm_running_app(camera_board_t *camera, esp_err_t audio_result);
