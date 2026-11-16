#pragma once

#include <stddef.h>
#include <stdint.h>

#define CAMERA_AUDIO_SAMPLE_RATE 16000
#define CAMERA_AUDIO_SAMPLE_COUNT 320
#define CAMERA_AUDIO_CHANNELS 1
#define CAMERA_AUDIO_BITS 16
#define CAMERA_AUDIO_HEADER_BYTES 24
#define CAMERA_AUDIO_PCM_BYTES (CAMERA_AUDIO_SAMPLE_COUNT * 2)
#define CAMERA_AUDIO_PACKET_BYTES (CAMERA_AUDIO_HEADER_BYTES + CAMERA_AUDIO_PCM_BYTES)

size_t camera_audio_write_packet(uint8_t *output, size_t capacity, uint32_t sequence,
                                 uint64_t sample_index, const uint8_t *pcm, size_t pcm_size);
size_t camera_audio_pcm_from_stereo32(const int32_t *input, size_t frames, uint8_t *output,
                                      size_t capacity, unsigned int channel);
