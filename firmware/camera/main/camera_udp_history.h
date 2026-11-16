#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define CAMERA_UDP_HISTORY_MAX_FRAMES 64
#define CAMERA_UDP_HISTORY_MAX_BYTES (4 * 1024 * 1024)
#define CAMERA_UDP_NACK_PACKET_SIZE 28

typedef struct {
    uint32_t sequence;
    uint16_t first_index;
    uint16_t index_count;
    uint16_t next_index;
    uint8_t mask[16];
} camera_udp_repair_cursor_t;

typedef struct {
    uint32_t frames;
    uint32_t bytes;
    uint32_t nack_requests;
    uint32_t retransmitted_packets;
    uint32_t drops;
} camera_udp_history_stats_t;

void camera_udp_history_init(void);
esp_err_t camera_udp_history_store(uint32_t sequence, const uint8_t *jpeg, uint32_t frame_size, uint32_t crc);
esp_err_t camera_udp_history_begin(const uint8_t *request, size_t request_size, camera_udp_repair_cursor_t *cursor);
bool camera_udp_history_next(camera_udp_repair_cursor_t *cursor, uint8_t *packet, size_t capacity, size_t *packet_size);
void camera_udp_history_stats(camera_udp_history_stats_t *stats);
