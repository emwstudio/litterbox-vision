#pragma once

#include <stdint.h>
#include "catvision_display.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t pushed_frames;
    uint32_t overlap_frames;
    uint32_t udp_completed;
    uint32_t dropped;
    uint32_t crc_errors;
    uint32_t stale;
    uint32_t incomplete;
    uint32_t no_slot;
    uint32_t queue_full;
    uint32_t pipeline_drops;
    int64_t decode_us;
    int64_t crc_us;
    int64_t push_us;
} catvision_live_stats_t;

esp_err_t catvision_live_start(catvision_image_t *image, int width, int height);
esp_err_t catvision_live_stats(catvision_live_stats_t *stats);

#ifdef __cplusplus
}
#endif
