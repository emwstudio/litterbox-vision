#include "live_pipeline.h"

#include <stdlib.h>
#include <string.h>
#include <atomic>
#include "catvision_crc.h"
#include "catvision_direct.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "udp_receiver.h"
#include "JPEGDEC.h"

#define PIXEL_BUFFER_COUNT 3

typedef struct {
    uint8_t *pixels;
    uint32_t seq;
    uint32_t crc;
    int64_t decode_us;
    int64_t crc_us;
} decoded_frame_t;

typedef struct {
    int width;
    int height;
    size_t bytes;
    catvision_image_t *image;
    JPEGDEC decoder;
    QueueHandle_t free_pixels;
    QueueHandle_t ready_pixels;
    uint8_t *pixel_buffers[PIXEL_BUFFER_COUNT];
    TaskHandle_t decode_task;
    TaskHandle_t present_task;
    std::atomic<bool> presenting{false};
    std::atomic<uint32_t> overlap_frames{0};
    std::atomic<uint32_t> pipeline_drops{0};
    std::atomic<uint32_t> frames{0};
    int64_t decode_us;
    int64_t crc_us;
    int64_t push_us;
} live_context_t;

static live_context_t live;

static int copy_pixels(JPEGDRAW *draw)
{
    uint16_t *output = static_cast<uint16_t *>(draw->pUser);
    for (int y = 0; y < draw->iHeight; y++) {
        int row = draw->y + y;
        if (row >= live.height) {
            break;
        }
        int width = draw->iWidth;
        if (draw->x + width > live.width) {
            width = live.width - draw->x;
        }
        if (width > 0) {
            memcpy(output + row * live.width + draw->x, draw->pPixels + y * draw->iWidth, width * 2);
        }
    }
    return 1;
}

static void decode_loop(void *argument)
{
    (void)argument;
    while (true) {
        const uint8_t *jpeg = NULL;
        size_t jpeg_size = 0;
        uint32_t seq = 0;
        if (udp_receiver_read(&jpeg, &jpeg_size, &seq, 1000) != ESP_OK) {
            continue;
        }
        uint8_t *pixels = NULL;
        if (xQueueReceive(live.free_pixels, &pixels, 0) != pdPASS) {
            live.pipeline_drops++;
            udp_receiver_release();
            continue;
        }
        if (live.presenting.load()) {
            live.overlap_frames++;
        }
        int64_t started = esp_timer_get_time();
        int opened = live.decoder.openRAM(const_cast<uint8_t *>(jpeg), static_cast<int>(jpeg_size), copy_pixels);
        int decoded = 0;
        if (opened) {
            live.decoder.setPixelType(RGB565_BIG_ENDIAN);
            live.decoder.setUserPointer(pixels);
            decoded = live.decoder.decode(0, 0, 0);
            live.decoder.close();
        }
        int64_t decoded_at = esp_timer_get_time();
        if (!opened || decoded != 1 || live.decoder.getWidth() != live.width || live.decoder.getHeight() != live.height) {
            ESP_LOGW("live", "JPEG decode failed open=%d decoded=%d error=%d bytes=%u",
                     opened, decoded, live.decoder.getLastError(), (unsigned int)jpeg_size);
            xQueueSend(live.free_pixels, &pixels, 0);
            udp_receiver_release();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        udp_receiver_release();
        decoded_frame_t frame = {
            .pixels = pixels,
            .seq = seq,
            .crc = 0,
            .decode_us = decoded_at - started,
            .crc_us = 0,
        };
        if (xQueueSend(live.ready_pixels, &frame, 0) != pdPASS) {
            live.pipeline_drops++;
            xQueueSend(live.free_pixels, &pixels, 0);
        }
        vTaskDelay(1);
    }
}

static void present_loop(void *argument)
{
    (void)argument;
    decoded_frame_t current = {};
    while (true) {
        if (xQueueReceive(live.ready_pixels, &current, portMAX_DELAY) != pdPASS) {
            continue;
        }
        uint8_t *previous = live.image->pixels;
        int64_t started = esp_timer_get_time();
        live.presenting.store(true);
        esp_err_t result = catvision_direct_update_owned_native(live.image, current.pixels, live.bytes, current.crc,
                                                                live.width, live.height);
        live.presenting.store(false);
        int64_t presented = esp_timer_get_time();
        if (result != ESP_OK) {
            xQueueSend(live.free_pixels, &current.pixels, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (previous && previous != current.pixels && previous != live.image->delta_base) {
            xQueueSend(live.free_pixels, &previous, 0);
        }
        live.decode_us = current.decode_us;
        live.crc_us = current.crc_us;
        live.push_us = presented - started;
        uint32_t frame = live.frames++;
        if (frame % 16 == 0) {
            catvision_live_stats_t stats = {};
            catvision_live_stats(&stats);
            ESP_LOGI("live", "pushed_frame=%lu decode_ms=%lld crc_ms=%lld push_ms=%lld seq=%lu overlap_frames=%lu udp_completed=%lu dropped=%lu crc_errors=%lu stale=%lu incomplete=%lu no_slot=%lu queue_full=%lu pipeline_drops=%lu",
                     (unsigned long)frame, stats.decode_us / 1000, stats.crc_us / 1000, stats.push_us / 1000,
                     (unsigned long)current.seq, (unsigned long)stats.overlap_frames,
                     (unsigned long)stats.udp_completed, (unsigned long)stats.dropped,
                     (unsigned long)stats.crc_errors, (unsigned long)stats.stale,
                     (unsigned long)stats.incomplete, (unsigned long)stats.no_slot,
                     (unsigned long)stats.queue_full, (unsigned long)stats.pipeline_drops);
        }
        vTaskDelay(1);
    }
}

static void release_pipeline(void)
{
    if (live.decode_task) {
        vTaskDeleteWithCaps(live.decode_task);
        live.decode_task = NULL;
    }
    if (live.present_task) {
        vTaskDeleteWithCaps(live.present_task);
        live.present_task = NULL;
    }
    if (live.free_pixels) {
        vQueueDelete(live.free_pixels);
        live.free_pixels = NULL;
    }
    if (live.ready_pixels) {
        vQueueDelete(live.ready_pixels);
        live.ready_pixels = NULL;
    }
    for (int index = 0; index < PIXEL_BUFFER_COUNT; index++) {
        free(live.pixel_buffers[index]);
        live.pixel_buffers[index] = NULL;
    }
    live.image = NULL;
}

esp_err_t catvision_live_start(catvision_image_t *image, int width, int height)
{
    if (!image || live.image || !((width == 466 && height == 466) || (width == 480 && height == 240))) {
        return ESP_ERR_INVALID_ARG;
    }
    live.width = width;
    live.height = height;
    live.bytes = (size_t)width * height * 2;
    live.image = image;
    live.free_pixels = xQueueCreate(PIXEL_BUFFER_COUNT, sizeof(uint8_t *));
    live.ready_pixels = xQueueCreate(PIXEL_BUFFER_COUNT, sizeof(decoded_frame_t));
    if (!live.free_pixels || !live.ready_pixels) {
        release_pipeline();
        return ESP_ERR_NO_MEM;
    }
    for (int index = 0; index < PIXEL_BUFFER_COUNT; index++) {
        live.pixel_buffers[index] = static_cast<uint8_t *>(heap_caps_malloc(live.bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!live.pixel_buffers[index] || xQueueSend(live.free_pixels, &live.pixel_buffers[index], 0) != pdPASS) {
            release_pipeline();
            return ESP_ERR_NO_MEM;
        }
    }
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(decode_loop, "live_decode", 8192, NULL, 5,
                                                          &live.decode_task, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        release_pipeline();
        return ESP_ERR_NO_MEM;
    }
    created = xTaskCreatePinnedToCoreWithCaps(present_loop, "live_present", 8192, NULL, 4,
                                               &live.present_task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        release_pipeline();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = udp_receiver_start();
    if (result != ESP_OK) {
        release_pipeline();
        return result;
    }
    ESP_LOGI("live", "pipeline width=%d height=%d jpeg_bytes=%u pixel_buffers=%d internal_free=%u internal_largest=%u",
             width, height, (unsigned int)sizeof(JPEGDEC), PIXEL_BUFFER_COUNT,
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

esp_err_t catvision_live_stats(catvision_live_stats_t *stats)
{
    if (!stats || !live.image) {
        return ESP_ERR_INVALID_STATE;
    }
    stats->pushed_frames = live.frames.load();
    stats->overlap_frames = live.overlap_frames.load();
    stats->pipeline_drops = live.pipeline_drops.load();
    stats->decode_us = live.decode_us;
    stats->crc_us = live.crc_us;
    stats->push_us = live.push_us;
    udp_receiver_stats(&stats->udp_completed, &stats->dropped, &stats->crc_errors,
                       &stats->stale, &stats->incomplete, &stats->no_slot, &stats->queue_full);
    return ESP_OK;
}
