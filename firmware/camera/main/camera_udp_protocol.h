#pragma once

#include <stddef.h>
#include <stdint.h>

#define CAMERA_UDP_HEADER_SIZE 20
#define CAMERA_UDP_CHUNK_SIZE 1400
#define CAMERA_UDP_PACKET_SIZE (CAMERA_UDP_HEADER_SIZE + CAMERA_UDP_CHUNK_SIZE)
#define CAMERA_UDP_FRAME_CAPACITY (128 * 1024)

uint32_t camera_udp_crc32(const uint8_t *data, size_t size);
size_t camera_udp_write_header(uint8_t *output, size_t capacity, uint32_t sequence, uint16_t index,
                               uint16_t count, uint32_t frame_size, uint32_t crc);
