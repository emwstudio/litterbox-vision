#include "camera_ota.h"

#include <string.h>
#include "camera_audio.h"
#include "camera_ota_url.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static TaskHandle_t ota_task;
static camera_ota_state_t ota_state;
static int32_t ota_bytes;
static esp_err_t ota_error;
static char ota_url[256];

static esp_err_t ota_http_client_init(esp_http_client_handle_t client)
{
    return esp_http_client_set_header(client, "X-Device-Token", CONFIG_CATVISION_DEVICE_TOKEN);
}

static void ota_download_task(void *argument)
{
    (void)argument;
    esp_http_client_config_t http_config = {
        .url = ota_url,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .keep_alive_enable = false,
    };
    esp_https_ota_config_t config = {
        .http_config = &http_config,
        .http_client_init_cb = ota_http_client_init,
    };
    esp_https_ota_handle_t handle = NULL;
    esp_err_t result = esp_https_ota_begin(&config, &handle);
    if (result == ESP_OK) {
        do {
            result = esp_https_ota_perform(handle);
            int bytes = esp_https_ota_get_image_len_read(handle);
            if (bytes >= 0) {
                __atomic_store_n(&ota_bytes, bytes, __ATOMIC_RELAXED);
            }
        } while (result == ESP_ERR_HTTPS_OTA_IN_PROGRESS);
        if (result == ESP_OK && !esp_https_ota_is_complete_data_received(handle)) {
            result = ESP_ERR_INVALID_SIZE;
        }
    }
    if (result == ESP_OK) {
        result = esp_https_ota_finish(handle);
    } else {
        if (handle) {
            esp_https_ota_abort(handle);
        }
    }
    if (result != ESP_OK) {
        __atomic_store_n(&ota_error, result, __ATOMIC_RELAXED);
        __atomic_store_n(&ota_state, CAMERA_OTA_ERROR, __ATOMIC_RELAXED);
        ESP_LOGE("camera_ota", "Download or image validation failed: %s", esp_err_to_name(result));
        __atomic_store_n(&ota_task, NULL, __ATOMIC_RELAXED);
        vTaskDelete(NULL);
        return;
    }
    __atomic_store_n(&ota_state, CAMERA_OTA_RESTARTING, __ATOMIC_RELAXED);
    ESP_LOGI("camera_ota", "Image accepted, bytes=%ld; restarting into the pending application", (long)ota_bytes);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

esp_err_t camera_ota_start(const char *url)
{
    if (!url || strlen(url) >= sizeof(ota_url) ||
        !camera_ota_url_allowed(url, CONFIG_CATVISION_UDP_HOST, CONFIG_CATVISION_OTA_SERVER_PORT)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(CONFIG_CATVISION_DEVICE_TOKEN) == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!__sync_bool_compare_and_swap(&ota_task, NULL, (TaskHandle_t)1)) {
        return ESP_ERR_INVALID_STATE;
    }
    strcpy(ota_url, url);
    __atomic_store_n(&ota_bytes, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&ota_error, ESP_OK, __ATOMIC_RELAXED);
    __atomic_store_n(&ota_state, CAMERA_OTA_DOWNLOADING, __ATOMIC_RELAXED);
    if (xTaskCreatePinnedToCore(ota_download_task, "camera_ota", 8192, NULL, 3, &ota_task, 1) != pdPASS) {
        __atomic_store_n(&ota_task, NULL, __ATOMIC_RELAXED);
        __atomic_store_n(&ota_error, ESP_ERR_NO_MEM, __ATOMIC_RELAXED);
        __atomic_store_n(&ota_state, CAMERA_OTA_ERROR, __ATOMIC_RELAXED);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void camera_ota_status(camera_ota_status_t *status)
{
    if (!status) {
        return;
    }
    status->state = __atomic_load_n(&ota_state, __ATOMIC_RELAXED);
    status->bytes = __atomic_load_n(&ota_bytes, __ATOMIC_RELAXED);
    status->error = __atomic_load_n(&ota_error, __ATOMIC_RELAXED);
}

const char *camera_ota_state_name(camera_ota_state_t state)
{
    switch (state) {
        case CAMERA_OTA_DOWNLOADING:
            return "downloading";
        case CAMERA_OTA_RESTARTING:
            return "restarting";
        case CAMERA_OTA_ERROR:
            return "error";
        default:
            return "idle";
    }
}

esp_err_t camera_ota_confirm_running_app(camera_board_t *camera, esp_err_t audio_result)
{
    if (!camera) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_ota_img_states_t image_state;
    esp_err_t result = esp_ota_get_state_partition(running, &image_state);
    if (result == ESP_ERR_NOT_FOUND || result == ESP_ERR_NOT_SUPPORTED) {
        return ESP_OK;
    }
    if (result != ESP_OK) {
        return result;
    }
    if (image_state != ESP_OTA_IMG_PENDING_VERIFY) {
        return ESP_OK;
    }
    bool valid = !CONFIG_CATVISION_OTA_SELF_TEST_FAILURE;
    uint32_t frames = 0;
    uint32_t packets = 0;
    for (int attempt = 0; attempt < 50 && valid; attempt++) {
        frames = __atomic_load_n(&camera->captured_frames, __ATOMIC_RELAXED);
        if (CONFIG_CATVISION_AUDIO_ENABLED) {
            uint32_t errors;
            int64_t last_packet_us;
            camera_audio_stats(&packets, &errors, &last_packet_us);
            if (audio_result != ESP_OK) {
                valid = false;
                break;
            }
        }
        if (frames > 0 && (!CONFIG_CATVISION_AUDIO_ENABLED || packets > 0)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    valid = valid && frames > 0 && (!CONFIG_CATVISION_AUDIO_ENABLED || packets > 0);
    if (!valid) {
        ESP_LOGE("camera_ota", "Pending image failed camera/audio self-test; rolling back");
        return esp_ota_mark_app_invalid_rollback_and_reboot();
    }
    ESP_LOGI("camera_ota", "Pending image passed camera/audio self-test");
    return esp_ota_mark_app_valid_cancel_rollback();
}
