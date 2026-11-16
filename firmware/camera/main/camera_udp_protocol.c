#include "camera_udp_protocol.h"

uint32_t camera_udp_crc32(const uint8_t *data, size_t size)
{
    if (!data && size != 0) {
        return 0;
    }
    uint32_t crc = 0xffffffff;
    for (size_t index = 0; index < size; index++) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
        }
    }
    return crc ^ 0xffffffff;
}

static void write_u16(uint8_t *output, uint16_t value)
{
    output[0] = value & 0xff;
    output[1] = value >> 8;
}

static void write_u32(uint8_t *output, uint32_t value)
{
    output[0] = value & 0xff;
    output[1] = (value >> 8) & 0xff;
    output[2] = (value >> 16) & 0xff;
    output[3] = value >> 24;
}

size_t camera_udp_write_header(uint8_t *output, size_t capacity, uint32_t sequence, uint16_t index,
                               uint16_t count, uint32_t frame_size, uint32_t crc)
{
    if (!output || capacity < CAMERA_UDP_HEADER_SIZE) {
        return 0;
    }
    output[0] = 'C';
    output[1] = 'V';
    output[2] = 'J';
    output[3] = '1';
    write_u32(output + 4, sequence);
    write_u16(output + 8, index);
    write_u16(output + 10, count);
    write_u32(output + 12, frame_size);
    write_u32(output + 16, crc);
    return CAMERA_UDP_HEADER_SIZE;
}
