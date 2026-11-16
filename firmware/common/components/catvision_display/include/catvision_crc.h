#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t catvision_crc32(const uint8_t *data, size_t size);
uint32_t catvision_crc32_combine(uint32_t first, uint32_t second, size_t second_size);

#ifdef __cplusplus
}
#endif
