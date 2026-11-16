#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "catvision_live_stream.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#ifdef ESP_PLATFORM
#include "freertos/task.h"
#endif

#define CATVISION_LIVE_CHANNEL_SLOTS 2

typedef struct {
    uint8_t *data;
    size_t size;
    uint32_t crc;
    bool delta;
} catvision_live_frame_t;

typedef struct {
    catvision_live_stream_t stream;
    catvision_live_frame_t slots[CATVISION_LIVE_CHANNEL_SLOTS];
    uint8_t write_index;
    uint8_t read_index;
    uint8_t consumer_index;
    bool has_consumer;
    bool connected;
    bool stopping;
    int port;
    size_t maximum;
    char url[64];
    char target[16];
    char token[129];
    SemaphoreHandle_t free_slots;
    SemaphoreHandle_t ready_slots;
    SemaphoreHandle_t stopped;
    esp_err_t last_error;
#ifdef ESP_PLATFORM
    TaskHandle_t receiver;
#endif
} catvision_live_channel_t;

esp_err_t catvision_live_channel_start(catvision_live_channel_t *channel, const char *url, int port, const char *target, const char *token, size_t max_bytes);
esp_err_t catvision_live_channel_read(catvision_live_channel_t *channel, catvision_live_frame_t *frame, uint32_t timeout_ms);
esp_err_t catvision_live_channel_release(catvision_live_channel_t *channel);
esp_err_t catvision_live_channel_stop(catvision_live_channel_t *channel);
