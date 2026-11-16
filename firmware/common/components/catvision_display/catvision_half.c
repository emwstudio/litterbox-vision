#include "catvision_half.h"
#include "catvision_crc.h"
#include "catvision_zlib.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#endif

#define HALF_RECORD_HEADER 16
#define HALF_MAX_PARTS 8

typedef struct {
    const uint8_t *input;
    size_t input_size;
    const uint8_t *previous;
    uint8_t *output;
    size_t output_size;
    bool delta;
    uint32_t expected_crc;
    uint32_t crc;
    esp_err_t result;
    int64_t inflate_us;
    int64_t xor_us;
    int64_t verify_us;
} half_job_t;

static half_job_t half_jobs[HALF_MAX_PARTS];
static bool half_busy;
#ifdef ESP_PLATFORM
static TaskHandle_t half_workers[2];
static TaskHandle_t half_caller;
static volatile int half_part_count;
static volatile int half_next;
static volatile int half_finished;
static portMUX_TYPE half_lock = portMUX_INITIALIZER_UNLOCKED;
#endif

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static IRAM_ATTR void half_decode_one(half_job_t *job)
{
    int64_t started = esp_timer_get_time();
    job->result = catvision_zlib_decompress(job->input, job->input_size, job->output, job->output_size);
    int64_t inflated = esp_timer_get_time();
    if (job->result == ESP_OK && job->delta) {
        if ((((uintptr_t)job->previous) | job->output_size) & 3U) {
            uint16_t *output = (uint16_t *)job->output;
            const uint16_t *previous = (const uint16_t *)job->previous;
            size_t words = job->output_size / 2;
            for (size_t index = 0; index < words; index++) {
                output[index] ^= previous[index];
            }
            for (size_t index = words * 2; index < job->output_size; index++) {
                job->output[index] ^= job->previous[index];
            }
        } else {
            uint32_t *output = (uint32_t *)job->output;
            const uint32_t *previous = (const uint32_t *)job->previous;
            size_t words = job->output_size / 4;
            for (size_t index = 0; index < words; index++) {
                output[index] ^= previous[index];
            }
        }
    }
    int64_t restored = esp_timer_get_time();
    job->crc = job->result == ESP_OK ? catvision_crc32(job->output, job->output_size) : 0;
    if (job->result == ESP_OK && job->crc != job->expected_crc) {
        job->result = ESP_ERR_INVALID_RESPONSE;
    }
    int64_t verified = esp_timer_get_time();
    job->inflate_us = inflated - started;
    job->xor_us = restored - inflated;
    job->verify_us = verified - restored;
    static uint32_t profile_samples;
    if (profile_samples++ % 30 == 0) {
        ESP_LOGI("profile", "part_stages_us inflate=%lld xor=%lld crc=%lld bytes=%u",
                 job->inflate_us, job->xor_us, job->verify_us, (unsigned int)job->output_size);
    }
}

#ifdef ESP_PLATFORM
static void half_worker(void *argument)
{
    (void)argument;
    bool logged_stack = false;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (true) {
            portENTER_CRITICAL(&half_lock);
            if (half_next >= half_part_count) {
                portEXIT_CRITICAL(&half_lock);
                break;
            }
            int index = half_next++;
            portEXIT_CRITICAL(&half_lock);
            half_decode_one(&half_jobs[index]);
            if (!logged_stack) {
                ESP_LOGI("profile", "half_stack_hwm=%u", (unsigned int)uxTaskGetStackHighWaterMark(NULL));
                logged_stack = true;
            }
            portENTER_CRITICAL(&half_lock);
            half_finished++;
            bool complete = half_finished == half_part_count;
            portEXIT_CRITICAL(&half_lock);
            if (complete) {
                xTaskNotifyGive(half_caller);
            }
        }
    }
}
#endif

esp_err_t catvision_half_init(void)
{
#ifdef ESP_PLATFORM
    if (half_workers[0] && half_workers[1]) {
        return ESP_OK;
    }
    if (half_workers[0] || half_workers[1]) {
        return ESP_ERR_INVALID_STATE;
    }
    for (int index = 0; index < 2; index++) {
        char name[12];
        snprintf(name, sizeof(name), "live_half%d", index);
        ESP_LOGI("half", "worker%d stack=16384 internal_free=%u internal_largest=%u", index,
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (xTaskCreatePinnedToCoreWithCaps(half_worker, name, 16384, NULL, 3, &half_workers[index], index, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            for (int created = 0; created < 2; created++) {
                if (half_workers[created]) {
                    vTaskDeleteWithCaps(half_workers[created]);
                    half_workers[created] = NULL;
                }
            }
            return ESP_ERR_NO_MEM;
        }
    }
#endif
    return ESP_OK;
}

esp_err_t catvision_half_decode_parts(const catvision_image_t *image, const uint8_t *data, size_t size, int parts, uint32_t expected_crc, int width, int height, uint8_t **decoded)
{
    if (!decoded) {
        return ESP_ERR_INVALID_ARG;
    }
    *decoded = NULL;
    if (!image || !data || (parts != 2 && parts != 4 && parts != 8) || size <= HALF_RECORD_HEADER * (size_t)parts || width <= 0 || height < parts) {
        return ESP_ERR_INVALID_ARG;
    }
    if (half_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t raw_size = (size_t)width * height * 2;
    const uint8_t *previous = image->delta_base ? image->delta_base : image->pixels;
    size_t offset = 0;
    for (int index = 0; index < parts; index++) {
        size_t start = (size_t)(height * index / parts) * width * 2;
        size_t end = (size_t)(height * (index + 1) / parts) * width * 2;
        size_t expected_size = end - start;
        if (size - offset < HALF_RECORD_HEADER) {
            return ESP_ERR_INVALID_ARG;
        }
        uint32_t input_size = read_u32(data + offset);
        uint32_t raw_record_size = read_u32(data + offset + 8);
        uint8_t flags = data[offset + 12];
        if (input_size == 0 || raw_record_size != expected_size || (flags & ~1U) != 0 ||
            data[offset + 13] != 0 || data[offset + 14] != 0 || data[offset + 15] != 0 ||
            input_size > size - offset - HALF_RECORD_HEADER || ((flags & 1U) && !previous)) {
            return ESP_ERR_INVALID_ARG;
        }
        half_jobs[index].input = data + offset + HALF_RECORD_HEADER;
        half_jobs[index].input_size = input_size;
        half_jobs[index].previous = previous ? previous + start : NULL;
        half_jobs[index].output_size = expected_size;
        half_jobs[index].delta = (flags & 1U) != 0;
        half_jobs[index].expected_crc = read_u32(data + offset + 4);
        half_jobs[index].crc = 0;
        half_jobs[index].result = ESP_ERR_INVALID_STATE;
        half_jobs[index].inflate_us = 0;
        half_jobs[index].xor_us = 0;
        half_jobs[index].verify_us = 0;
        offset += HALF_RECORD_HEADER + input_size;
    }
    if (offset != size) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *buffer = malloc(raw_size);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }
    for (int index = 0; index < parts; index++) {
        size_t start = (size_t)(height * index / parts) * width * 2;
        half_jobs[index].output = buffer + start;
    }
    half_busy = true;
    int64_t started = esp_timer_get_time();
    esp_err_t result = ESP_OK;
#ifdef ESP_PLATFORM
    if (!half_workers[0] || !half_workers[1]) {
        half_busy = false;
        free(buffer);
        return ESP_ERR_INVALID_STATE;
    }
    half_part_count = parts;
    half_next = 0;
    half_finished = 0;
    half_caller = xTaskGetCurrentTaskHandle();
    xTaskNotifyGive(half_workers[0]);
    xTaskNotifyGive(half_workers[1]);
    while (half_finished < half_part_count) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
#else
    for (int index = 0; index < parts; index++) {
        half_decode_one(&half_jobs[index]);
    }
#endif
    for (int index = 0; index < parts; index++) {
        if (result == ESP_OK && half_jobs[index].result != ESP_OK) {
            result = half_jobs[index].result;
        }
    }
    uint32_t actual_crc = 0;
    if (result == ESP_OK) {
        actual_crc = half_jobs[0].crc;
        for (int index = 1; index < parts; index++) {
            actual_crc = catvision_crc32_combine(actual_crc, half_jobs[index].crc, half_jobs[index].output_size);
        }
        if (actual_crc != expected_crc) {
            result = ESP_ERR_INVALID_RESPONSE;
        }
    }
    half_busy = false;
    if (result == ESP_OK) {
        *decoded = buffer;
        static uint32_t profile_samples;
        if (profile_samples++ % 30 == 0) {
            ESP_LOGI("profile", "parts_decode_us=%lld parts=%d part0_us=%lld part1_us=%lld part2_us=%lld part3_us=%lld",
                     esp_timer_get_time() - started, parts,
                     half_jobs[0].inflate_us + half_jobs[0].xor_us + half_jobs[0].verify_us,
                     half_jobs[1].inflate_us + half_jobs[1].xor_us + half_jobs[1].verify_us,
                     half_jobs[2].inflate_us + half_jobs[2].xor_us + half_jobs[2].verify_us,
                     half_jobs[3].inflate_us + half_jobs[3].xor_us + half_jobs[3].verify_us);
        }
    } else {
        ESP_LOGW("display", "parts decode result=%d expected_crc=%lu decoded_crc=%lu current_crc=%lu",
                 result, (unsigned long)expected_crc, (unsigned long)actual_crc,
                 (unsigned long)image->source_crc);
        free(buffer);
    }
    return result;
}

esp_err_t catvision_half_decode(const catvision_image_t *image, const uint8_t *data, size_t size, uint32_t expected_crc, int width, int height, uint8_t **decoded)
{
    return catvision_half_decode_parts(image, data, size, 2, expected_crc, width, height, decoded);
}

esp_err_t catvision_parts_decode(const catvision_image_t *image, const uint8_t *data, size_t size, uint32_t expected_crc, int width, int height, uint8_t **decoded)
{
    return catvision_half_decode_parts(image, data, size, 4, expected_crc, width, height, decoded);
}
