#include "catvision_zlib.h"

#include "zlib.h"
#include "esp_log.h"

esp_err_t catvision_zlib_decompress(const uint8_t *compressed, size_t compressed_size, uint8_t *output, size_t output_size)
{
    if (!compressed || !output || compressed_size == 0 || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uLongf written = output_size;
    uLong consumed = compressed_size;
    int result = uncompress2(output, &written, compressed, &consumed);
    if (result != Z_OK || written != output_size || consumed != compressed_size) {
        ESP_LOGW("zlib", "result=%d written=%lu consumed=%lu input=%u output=%u header=%02x%02x",
                 result, (unsigned long)written, (unsigned long)consumed,
                 (unsigned int)compressed_size, (unsigned int)output_size, compressed[0], compressed[1]);
    }
    return result == Z_OK && written == output_size && consumed == compressed_size ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
