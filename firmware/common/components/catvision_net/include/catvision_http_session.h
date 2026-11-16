#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_client.h"

typedef struct {
    esp_http_client_handle_t client;
    uint8_t *data;
    size_t size;
    size_t capacity;
    size_t maximum;
    bool delta;
    bool has_frame_crc;
    uint32_t frame_crc;
    int64_t request_started_us;
    int64_t connected_us;
    int64_t headers_sent_us;
    int64_t first_data_us;
    unsigned int data_events;
} catvision_http_session_t;

esp_err_t catvision_http_session_open(catvision_http_session_t *session, const char *url, const char *token, size_t max_bytes);
esp_err_t catvision_http_session_set_frame_crc(catvision_http_session_t *session, uint32_t crc);
esp_err_t catvision_http_session_get(catvision_http_session_t *session, uint8_t **data, size_t *size);
void catvision_http_session_close(catvision_http_session_t *session);
