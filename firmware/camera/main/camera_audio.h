#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#ifndef CONFIG_CATVISION_AUDIO_ENABLED
#define CONFIG_CATVISION_AUDIO_ENABLED 0
#endif

esp_err_t camera_audio_start(void);
void camera_audio_stats(uint32_t *packets, uint32_t *errors, int64_t *last_packet_us);
