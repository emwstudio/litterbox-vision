#include "stream_server.h"

#include <stdio.h>
#include <string.h>

size_t camera_stream_pack_frame(uint8_t *output, size_t capacity, const uint8_t *jpeg, size_t jpeg_size)
{
    if (!output || !jpeg || jpeg_size == 0 || jpeg_size > UINT32_MAX) {
        return 0;
    }
    int header_size = snprintf((char *)output, capacity,
                               "\r\n--catvision-frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                               (unsigned int)jpeg_size);
    if (header_size < 0 || (size_t)header_size > capacity || jpeg_size > capacity - (size_t)header_size) {
        return 0;
    }
    memmove(output + header_size, jpeg, jpeg_size);
    return (size_t)header_size + jpeg_size;
}
