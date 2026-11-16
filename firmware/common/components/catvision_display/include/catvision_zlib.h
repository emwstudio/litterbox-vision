#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t catvision_zlib_decompress(const uint8_t *compressed, size_t compressed_size, uint8_t *output, size_t output_size);
