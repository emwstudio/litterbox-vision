#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    int socket_fd;
    uint8_t *data;
    size_t capacity;
    size_t maximum;
} catvision_live_stream_t;

esp_err_t catvision_live_stream_open(catvision_live_stream_t *stream, const char *url, int port, const char *target, const char *token, size_t max_bytes);
esp_err_t catvision_live_stream_read(catvision_live_stream_t *stream, uint8_t **data, size_t *size, uint32_t *frame_crc, bool *delta);
esp_err_t catvision_live_stream_read_buffer(catvision_live_stream_t *stream, uint8_t *data, size_t capacity, size_t *size, uint32_t *frame_crc, bool *delta);
esp_err_t catvision_live_stream_ack(catvision_live_stream_t *stream, uint32_t frame_crc);
void catvision_live_stream_close(catvision_live_stream_t *stream);
