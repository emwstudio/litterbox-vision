#pragma once

#include "camera_board.h"
#include "esp_err.h"

typedef struct {
    uint32_t sent_frames;
    uint32_t sent_packets;
    uint32_t send_errors;
    uint32_t send_aborts;
    uint32_t retry_successes;
    uint32_t sequence;
    uint32_t capture_sequence;
    uint32_t frame_bytes;
    int last_socket_error;
    uint32_t last_error_sequence;
    uint32_t last_error_index;
} camera_udp_status_t;

struct sockaddr_in;

void camera_udp_stats(camera_udp_status_t *status);
esp_err_t camera_udp_send_packet(int socket_fd, const uint8_t *packet, size_t size,
                                 const struct sockaddr_in *destination, uint32_t sequence, uint16_t index);
esp_err_t camera_udp_stream_start(camera_board_t *camera);
