#include "catvision_live_channel.h"

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#ifdef ESP_PLATFORM
#include "freertos/idf_additions.h"
#endif

static void *channel_buffer_alloc(size_t size)
{
#ifdef ESP_PLATFORM
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(size);
#endif
}

static void channel_free_resources(catvision_live_channel_t *channel)
{
    if (channel->connected) {
        catvision_live_stream_close(&channel->stream);
        channel->connected = false;
    }
    for (int index = 0; index < CATVISION_LIVE_CHANNEL_SLOTS; index++) {
        free(channel->slots[index].data);
        channel->slots[index].data = NULL;
    }
    if (channel->free_slots) {
        vSemaphoreDelete(channel->free_slots);
        channel->free_slots = NULL;
    }
    if (channel->ready_slots) {
        vSemaphoreDelete(channel->ready_slots);
        channel->ready_slots = NULL;
    }
    if (channel->stopped) {
        vSemaphoreDelete(channel->stopped);
        channel->stopped = NULL;
    }
}

static esp_err_t channel_receive_one(catvision_live_channel_t *channel)
{
    if (!channel->connected) {
        esp_err_t result = catvision_live_stream_open(&channel->stream, channel->url, channel->port,
                                                       channel->target, channel->token, channel->maximum);
        if (result != ESP_OK) {
            channel->last_error = result;
            return result;
        }
        channel->connected = true;
        ESP_LOGI("channel", "connected target=%s", channel->target);
    }
    if (xSemaphoreTake(channel->free_slots, pdMS_TO_TICKS(100)) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    catvision_live_frame_t *slot = &channel->slots[channel->write_index];
    int64_t started = esp_timer_get_time();
    esp_err_t result = catvision_live_stream_read_buffer(&channel->stream, slot->data, channel->maximum,
                                                          &slot->size, &slot->crc, &slot->delta);
    int64_t ack_started = started;
    int64_t acknowledged = started;
    if (result == ESP_OK) {
        ack_started = esp_timer_get_time();
        result = catvision_live_stream_ack(&channel->stream, slot->crc);
        acknowledged = esp_timer_get_time();
    }
    if (result != ESP_OK) {
        channel->last_error = result;
        catvision_live_stream_close(&channel->stream);
        channel->connected = false;
        xSemaphoreGive(channel->free_slots);
        return result;
    }
    channel->write_index = (channel->write_index + 1) % CATVISION_LIVE_CHANNEL_SLOTS;
    channel->last_error = ESP_OK;
    xSemaphoreGive(channel->ready_slots);
    static uint32_t profile_samples;
    if (profile_samples++ % 30 == 0) {
        ESP_LOGI("profile", "receive_us=%lld ack_us=%lld", acknowledged - started, acknowledged - ack_started);
    }
    (void)started;
    return ESP_OK;
}

#ifdef ESP_PLATFORM
static void channel_worker(void *argument)
{
    catvision_live_channel_t *channel = argument;
    while (!channel->stopping) {
        esp_err_t result = channel_receive_one(channel);
        if (result != ESP_OK && result != ESP_ERR_TIMEOUT) {
            ESP_LOGW("channel", "receive failed target=%s result=%d", channel->target, result);
        }
        if (!channel->connected && !channel->stopping) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    xSemaphoreGive(channel->stopped);
    vTaskDeleteWithCaps(NULL);
}
#endif

esp_err_t catvision_live_channel_start(catvision_live_channel_t *channel, const char *url, int port, const char *target, const char *token, size_t max_bytes)
{
    if (!channel || !url || !target || !token || max_bytes == 0 || port <= 0 || port > 65535 ||
        strlen(url) >= sizeof(channel->url) || strlen(target) >= sizeof(channel->target) || strlen(token) >= sizeof(channel->token)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(channel, 0, sizeof(*channel));
    channel->port = port;
    channel->maximum = max_bytes;
    strcpy(channel->url, url);
    strcpy(channel->target, target);
    strcpy(channel->token, token);
    for (int index = 0; index < CATVISION_LIVE_CHANNEL_SLOTS; index++) {
        channel->slots[index].data = channel_buffer_alloc(max_bytes);
        if (!channel->slots[index].data) {
            channel_free_resources(channel);
            return ESP_ERR_NO_MEM;
        }
    }
    channel->free_slots = xSemaphoreCreateCounting(CATVISION_LIVE_CHANNEL_SLOTS, CATVISION_LIVE_CHANNEL_SLOTS);
    channel->ready_slots = xSemaphoreCreateCounting(CATVISION_LIVE_CHANNEL_SLOTS, 0);
    channel->stopped = xSemaphoreCreateBinary();
    if (!channel->free_slots || !channel->ready_slots || !channel->stopped) {
        channel_free_resources(channel);
        return ESP_ERR_NO_MEM;
    }
#ifdef ESP_PLATFORM
    if (xTaskCreatePinnedToCoreWithCaps(channel_worker, "live_receive", 8192, channel, 4, &channel->receiver, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        channel_free_resources(channel);
        return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}

esp_err_t catvision_live_channel_read(catvision_live_channel_t *channel, catvision_live_frame_t *frame, uint32_t timeout_ms)
{
    if (!channel || !frame || !channel->ready_slots || channel->has_consumer) {
        return ESP_ERR_INVALID_ARG;
    }
#ifndef ESP_PLATFORM
    if (xSemaphoreTake(channel->ready_slots, 0) != pdPASS) {
        esp_err_t result = channel_receive_one(channel);
        if (result != ESP_OK) {
            return result;
        }
    }
#endif
    if (xSemaphoreTake(channel->ready_slots, pdMS_TO_TICKS(timeout_ms)) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    channel->consumer_index = channel->read_index;
    channel->read_index = (channel->read_index + 1) % CATVISION_LIVE_CHANNEL_SLOTS;
    channel->has_consumer = true;
    *frame = channel->slots[channel->consumer_index];
    return ESP_OK;
}

esp_err_t catvision_live_channel_release(catvision_live_channel_t *channel)
{
    if (!channel || !channel->has_consumer) {
        return ESP_ERR_INVALID_ARG;
    }
    channel->has_consumer = false;
    xSemaphoreGive(channel->free_slots);
    return ESP_OK;
}

esp_err_t catvision_live_channel_stop(catvision_live_channel_t *channel)
{
    if (!channel) {
        return ESP_ERR_INVALID_ARG;
    }
    channel->stopping = true;
#ifdef ESP_PLATFORM
    if (channel->receiver) {
        if (channel->connected) {
            catvision_live_stream_close(&channel->stream);
            channel->connected = false;
        }
        xSemaphoreGive(channel->free_slots);
        if (xSemaphoreTake(channel->stopped, pdMS_TO_TICKS(1000)) != pdPASS) {
            return ESP_ERR_TIMEOUT;
        }
        channel->receiver = NULL;
    }
#endif
    channel_free_resources(channel);
    return ESP_OK;
}
