#pragma once

#include <stddef.h>
#include <stdint.h>
#include "catvision_display.h"

esp_err_t catvision_half_init(void);
esp_err_t catvision_half_decode(const catvision_image_t *image, const uint8_t *data, size_t size, uint32_t expected_crc, int width, int height, uint8_t **decoded);
esp_err_t catvision_half_decode_parts(const catvision_image_t *image, const uint8_t *data, size_t size, int parts, uint32_t expected_crc, int width, int height, uint8_t **decoded);
esp_err_t catvision_parts_decode(const catvision_image_t *image, const uint8_t *data, size_t size, uint32_t expected_crc, int width, int height, uint8_t **decoded);
