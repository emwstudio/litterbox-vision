#include "catvision_crc.h"

#include <stdbool.h>
#include "esp_attr.h"
#include "zlib.h"

uint32_t IRAM_ATTR catvision_crc32(const uint8_t *data, size_t size)
{
    static uint32_t table[256];
    static bool ready;
    if (!ready) {
        for (uint32_t value = 0; value < 256; value++) {
            uint32_t entry = value;
            for (int bit = 0; bit < 8; bit++) {
                entry = entry & 1 ? 0xedb88320U ^ (entry >> 1) : entry >> 1;
            }
            table[value] = entry;
        }
        ready = true;
    }
    uint32_t crc = 0xffffffffU;
    for (size_t index = 0; index < size; index++) {
        crc = table[(crc ^ data[index]) & 0xff] ^ (crc >> 8);
    }
    return ~crc;
}

uint32_t catvision_crc32_combine(uint32_t first, uint32_t second, size_t second_size)
{
    return (uint32_t)crc32_combine(first, second, (z_off_t)second_size);
}
