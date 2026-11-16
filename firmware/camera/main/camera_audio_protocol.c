#include "camera_audio_protocol.h"

#include <string.h>

static void write_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8);
    output[1] = (uint8_t)value;
}

static void write_u32(uint8_t *output, uint32_t value)
{
    for (int index = 0; index < 4; index++) {
        output[index] = (uint8_t)(value >> (24 - index * 8));
    }
}

static void write_u64(uint8_t *output, uint64_t value)
{
    for (int index = 0; index < 8; index++) {
        output[index] = (uint8_t)(value >> (56 - index * 8));
    }
}

size_t camera_audio_write_packet(uint8_t *output, size_t capacity, uint32_t sequence,
                                 uint64_t sample_index, const uint8_t *pcm, size_t pcm_size)
{
    if (!output || !pcm || pcm_size != CAMERA_AUDIO_PCM_BYTES || capacity < CAMERA_AUDIO_PACKET_BYTES) {
        return 0;
    }
    memcpy(output, "CVA1", 4);
    write_u32(output + 4, sequence);
    write_u64(output + 8, sample_index);
    write_u32(output + 16, CAMERA_AUDIO_SAMPLE_RATE);
    write_u16(output + 20, CAMERA_AUDIO_SAMPLE_COUNT);
    output[22] = CAMERA_AUDIO_CHANNELS;
    output[23] = CAMERA_AUDIO_BITS;
    memcpy(output + CAMERA_AUDIO_HEADER_BYTES, pcm, CAMERA_AUDIO_PCM_BYTES);
    return CAMERA_AUDIO_PACKET_BYTES;
}

size_t camera_audio_pcm_from_stereo32(const int32_t *input, size_t frames, uint8_t *output,
                                      size_t capacity, unsigned int channel)
{
    if (!input || !output || frames != CAMERA_AUDIO_SAMPLE_COUNT ||
        capacity < CAMERA_AUDIO_PCM_BYTES || channel > 1) {
        return 0;
    }
    for (size_t frame = 0; frame < frames; frame++) {
        uint16_t sample = (uint16_t)((uint32_t)input[frame * 2 + channel] >> 16);
        output[frame * 2] = (uint8_t)sample;
        output[frame * 2 + 1] = (uint8_t)(sample >> 8);
    }
    return CAMERA_AUDIO_PCM_BYTES;
}
