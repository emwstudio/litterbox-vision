#pragma once

#include "camera_board.h"
#include "esp_err.h"

esp_err_t stream_server_start(camera_board_t *camera);
size_t camera_stream_pack_frame(uint8_t *output, size_t capacity, const uint8_t *jpeg, size_t jpeg_size);
