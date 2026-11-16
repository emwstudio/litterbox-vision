#include "stream_server.h"
#include "cJSON.h"
#include "camera_audio.h"
#include "esp_app_desc.h"
#include "camera_ota.h"
#include "camera_udp_history.h"
#include "camera_udp_stream.h"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define PART_BOUNDARY "catvision-frame"
#define NO_FRAME_TIMEOUT_US 6000000

static const char *TAG = "stream_server";
static SemaphoreHandle_t stream_mutex;
static uint32_t stream_frames_sent;
static uint32_t stream_errors;
static uint32_t stream_sessions;

static esp_err_t send_error(httpd_req_t *request, const char *status, const char *message)
{
    httpd_resp_set_status(request, status);
    return httpd_resp_sendstr(request, message);
}

static bool authorized(httpd_req_t *request)
{
    if (strlen(CONFIG_CATVISION_DEVICE_TOKEN) == 0) {
        send_error(request, "503 Service Unavailable", "DEVICE_TOKEN is not configured");
        return false;
    }
    char supplied[128];
    if (httpd_req_get_hdr_value_str(request, "X-Device-Token", supplied, sizeof(supplied)) != ESP_OK ||
        strcmp(supplied, CONFIG_CATVISION_DEVICE_TOKEN) != 0) {
        send_error(request, "401 Unauthorized", "Invalid device token");
        return false;
    }
    return true;
}

static esp_err_t health_handler(httpd_req_t *request)
{
    if (!authorized(request)) {
        return ESP_FAIL;
    }
    camera_board_t *camera = request->user_ctx;
    xSemaphoreTake(camera->frame_lock, portMAX_DELAY);
    int64_t last_frame_us = camera->last_frame_us;
    uint32_t captured_frames = camera->captured_frames;
    uint32_t capture_errors = camera->capture_errors;
    uint32_t fps_milli = (uint32_t)(camera->measured_fps * 1000.0f + 0.5f);
    xSemaphoreGive(camera->frame_lock);
    int64_t age_ms = last_frame_us ? (esp_timer_get_time() - last_frame_us) / 1000 : -1;
    bool online = last_frame_us && age_ms >= 0 && age_ms < 3000;
    uint32_t audio_packets;
    uint32_t audio_errors;
    int64_t audio_last_us;
    camera_audio_stats(&audio_packets, &audio_errors, &audio_last_us);
    int64_t audio_age_ms = audio_last_us ? (esp_timer_get_time() - audio_last_us) / 1000 : -1;
    camera_ota_status_t ota;
    camera_ota_status(&ota);
    camera_udp_status_t udp;
    camera_udp_stats(&udp);
    camera_udp_history_stats_t history;
    camera_udp_history_stats(&history);
    char body[1152];
    int length = snprintf(body, sizeof(body),
                          "{\"online\":%s,\"width\":%lu,\"height\":%lu,\"fps\":%lu.%03lu,"
                          "\"last_frame_age_ms\":%lld,\"captured_frames\":%lu,\"capture_errors\":%lu,"
                          "\"stream_frames_sent\":%lu,\"stream_sessions\":%lu,\"stream_errors\":%lu,"
                          "\"audio_packets\":%lu,\"audio_errors\":%lu,\"audio_last_packet_age_ms\":%lld,"
                          "\"udp_sent_frames\":%lu,\"udp_packets_sent\":%lu,\"udp_send_errors\":%lu,"
                          "\"udp_sequence\":%lu,\"udp_capture_sequence\":%lu,\"udp_frame_bytes\":%lu,"
                          "\"udp_last_socket_error\":%d,\"udp_send_aborts\":%lu,\"udp_retry_successes\":%lu,"
                          "\"udp_last_error_sequence\":%lu,\"udp_last_error_index\":%lu,"
                          "\"udp_history_frames\":%lu,\"udp_history_bytes\":%lu,\"nack_requests\":%lu,"
                          "\"retransmitted_packets\":%lu,\"history_drops\":%lu,"
                          "\"firmware_version\":\"%s\",\"ota_state\":\"%s\",\"ota_bytes\":%ld,\"ota_error\":%d}",
                          online ? "true" : "false", camera->width, camera->height,
                          (unsigned long)(fps_milli / 1000), (unsigned long)(fps_milli % 1000),
                          (long long)age_ms, (unsigned long)captured_frames, (unsigned long)capture_errors,
                          (unsigned long)__atomic_load_n(&stream_frames_sent, __ATOMIC_RELAXED),
                          (unsigned long)__atomic_load_n(&stream_sessions, __ATOMIC_RELAXED),
                          (unsigned long)__atomic_load_n(&stream_errors, __ATOMIC_RELAXED),
                          (unsigned long)audio_packets, (unsigned long)audio_errors, (long long)audio_age_ms,
                          (unsigned long)udp.sent_frames, (unsigned long)udp.sent_packets,
                          (unsigned long)udp.send_errors, (unsigned long)udp.sequence,
                          (unsigned long)udp.capture_sequence, (unsigned long)udp.frame_bytes,
                          udp.last_socket_error, (unsigned long)udp.send_aborts,
                          (unsigned long)udp.retry_successes, (unsigned long)udp.last_error_sequence,
                          (unsigned long)udp.last_error_index,
                          (unsigned long)history.frames, (unsigned long)history.bytes,
                          (unsigned long)history.nack_requests, (unsigned long)history.retransmitted_packets,
                          (unsigned long)history.drops, esp_app_get_description()->version,
                          camera_ota_state_name(ota.state), (long)ota.bytes, (int)ota.error);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, body, length);
}

static esp_err_t snapshot_handler(httpd_req_t *request)
{
    if (!authorized(request)) {
        return ESP_FAIL;
    }
    camera_board_t *camera = request->user_ctx;
    uint8_t *jpeg = heap_caps_malloc(CAMERA_JPEG_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg) {
        send_error(request, "503 Service Unavailable", "No frame buffer available");
        return ESP_ERR_NO_MEM;
    }
    uint32_t sequence = 0;
    size_t size = 0;
    esp_err_t result = camera_board_copy_latest(camera, jpeg, CAMERA_JPEG_CAP, &sequence, &size);
    if (result != ESP_OK) {
        heap_caps_free(jpeg);
        send_error(request, "503 Service Unavailable", "Camera capture unavailable");
        return result;
    }
    httpd_resp_set_type(request, "image/jpeg");
    result = httpd_resp_send(request, (const char *)jpeg, size);
    heap_caps_free(jpeg);
    return result;
}

static void stream_session_task(void *argument)
{
    httpd_req_t *request = argument;
    if (!authorized(request)) {
        return;
    }
    if (xSemaphoreTake(stream_mutex, 0) != pdPASS) {
        send_error(request, "503 Service Unavailable", "Another stream is already active");
        return;
    }
    camera_board_t *camera = request->user_ctx;
    uint8_t *jpeg = heap_caps_malloc(CAMERA_JPEG_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *packet = heap_caps_malloc(CAMERA_JPEG_CAP + 128, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg || !packet) {
        heap_caps_free(jpeg);
        heap_caps_free(packet);
        xSemaphoreGive(stream_mutex);
        send_error(request, "503 Service Unavailable", "No stream buffer available");
        return;
    }
    __atomic_add_fetch(&stream_sessions, 1, __ATOMIC_RELAXED);
    httpd_resp_set_type(request, "multipart/x-mixed-replace;boundary=" PART_BOUNDARY);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t result = ESP_OK;
    uint32_t sequence = 0;
    uint32_t profile_frames = 0;
    int64_t no_frame_since = esp_timer_get_time();
    while (true) {
        int64_t started = esp_timer_get_time();
        size_t size = 0;
        result = camera_board_copy_latest(camera, jpeg, CAMERA_JPEG_CAP, &sequence, &size);
        if (result == ESP_ERR_NOT_FOUND) {
            if (esp_timer_get_time() - no_frame_since > NO_FRAME_TIMEOUT_US) {
                __atomic_add_fetch(&stream_errors, 1, __ATOMIC_RELAXED);
                result = ESP_ERR_TIMEOUT;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (result != ESP_OK) {
            __atomic_add_fetch(&stream_errors, 1, __ATOMIC_RELAXED);
            break;
        }
        int64_t copied = esp_timer_get_time();
        size_t packet_size = camera_stream_pack_frame(packet, CAMERA_JPEG_CAP + 128, jpeg, size);
        if (packet_size == 0) {
            __atomic_add_fetch(&stream_errors, 1, __ATOMIC_RELAXED);
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        no_frame_since = esp_timer_get_time();
        if (httpd_resp_send_chunk(request, (const char *)packet, packet_size) != ESP_OK) {
            __atomic_add_fetch(&stream_errors, 1, __ATOMIC_RELAXED);
            result = ESP_FAIL;
            break;
        }
        __atomic_add_fetch(&stream_frames_sent, 1, __ATOMIC_RELAXED);
        if (profile_frames++ % 16 == 0) {
            ESP_LOGI("camera_profile", "copy_us=%lld send_us=%lld total_us=%lld bytes=%u",
                     copied - started, esp_timer_get_time() - copied, esp_timer_get_time() - started,
                     (unsigned int)size);
        }
    }
    heap_caps_free(jpeg);
    heap_caps_free(packet);
    xSemaphoreGive(stream_mutex);
    httpd_req_async_handler_complete(request);
    vTaskDelete(NULL);
}

static esp_err_t stream_handler(httpd_req_t *request)
{
    if (!authorized(request)) {
        return ESP_FAIL;
    }
    httpd_req_t *async_request = NULL;
    if (httpd_req_async_handler_begin(request, &async_request) != ESP_OK) {
        return ESP_FAIL;
    }
    if (xTaskCreatePinnedToCore(stream_session_task, "camera_stream", 8192, async_request, 2, NULL, 1) != pdPASS) {
        send_error(async_request, "503 Service Unavailable", "No stream task available");
        httpd_req_async_handler_complete(async_request);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t ota_post_handler(httpd_req_t *request)
{
    if (!authorized(request)) {
        return ESP_FAIL;
    }
    char body[256];
    int received = httpd_req_recv(request, body, sizeof(body) - 1);
    if (received <= 0) {
        send_error(request, "400 Bad Request", "OTA URL is required");
        return ESP_FAIL;
    }
    body[received] = '\0';
    cJSON *document = cJSON_Parse(body);
    const char *url = document ? cJSON_GetStringValue(cJSON_GetObjectItem(document, "url")) : NULL;
    if (!url) {
        cJSON_Delete(document);
        send_error(request, "400 Bad Request", "JSON body must contain a URL string");
        return ESP_FAIL;
    }
    esp_err_t result = camera_ota_start(url);
    cJSON_Delete(document);
    httpd_resp_set_type(request, "application/json");
    if (result == ESP_OK) {
        httpd_resp_set_status(request, "202 Accepted");
        return httpd_resp_sendstr(request, "{\"state\":\"downloading\"}");
    }
    if (result == ESP_ERR_INVALID_STATE) {
        send_error(request, "409 Conflict", "An OTA task is already running or is not configured");
    } else {
        send_error(request, "400 Bad Request", esp_err_to_name(result));
    }
    return ESP_FAIL;
}

esp_err_t stream_server_start(camera_board_t *camera)
{
    stream_mutex = xSemaphoreCreateMutex();
    if (!stream_mutex) {
        return ESP_ERR_NO_MEM;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_open_sockets = 5;
    config.lru_purge_enable = true;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        return ESP_FAIL;
    }
    const httpd_uri_t health = {.uri = "/health", .method = HTTP_GET, .handler = health_handler, .user_ctx = camera};
    const httpd_uri_t snapshot = {.uri = "/snapshot", .method = HTTP_GET, .handler = snapshot_handler, .user_ctx = camera};
    const httpd_uri_t stream = {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = camera};
    const httpd_uri_t ota = {.uri = "/ota", .method = HTTP_POST, .handler = ota_post_handler, .user_ctx = camera};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &health));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &snapshot));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &stream));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ota));
    esp_err_t udp_result = camera_udp_stream_start(camera);
    if (udp_result != ESP_OK) {
        ESP_LOGW(TAG, "UDP preview unavailable: %s", esp_err_to_name(udp_result));
    }
    ESP_LOGI(TAG, "Camera HTTP endpoints ready");
    return ESP_OK;
}
