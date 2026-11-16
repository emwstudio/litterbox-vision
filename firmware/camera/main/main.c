#include "camera_board.h"
#include "camera_audio.h"
#include "camera_ota.h"
#include "stream_server.h"

#include "bsp/esp32_s3_cam_ovxxxx.h"
#include "custom_io_expander_ch32v003.h"
#include "driver/i2c_master.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "protocol_examples_common.h"

static camera_board_t camera;

static esp_err_t initialize_io_expander(void)
{
    i2c_master_bus_handle_t i2c;
    ESP_ERROR_CHECK(i2c_master_get_bus_handle(0, &i2c));
    esp_io_expander_handle_t expander;
    ESP_ERROR_CHECK(custom_io_expander_new_i2c_ch32v003(i2c, CUSTOM_IO_EXPANDER_I2C_CH32V003_ADDRESS, &expander));
    ESP_ERROR_CHECK(esp_io_expander_set_dir(expander,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2 |
        IO_EXPANDER_PIN_NUM_4 | IO_EXPANDER_PIN_NUM_5 | IO_EXPANDER_PIN_NUM_6, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander,
        IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2 |
        IO_EXPANDER_PIN_NUM_5 | IO_EXPANDER_PIN_NUM_6, 1));
    ESP_ERROR_CHECK(esp_io_expander_set_level(expander, IO_EXPANDER_PIN_NUM_4, 0));
    return esp_io_expander_del(expander);
}

void app_main(void)
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    esp_task_wdt_config_t watchdog = {
        .timeout_ms = 10000,
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic = true,
    };
    esp_err_t watchdog_result = esp_task_wdt_init(&watchdog);
    if (watchdog_result != ESP_OK && watchdog_result != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(watchdog_result);
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("catvision-camera"));

    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_ERROR_CHECK(initialize_io_expander());
    ESP_ERROR_CHECK(camera_board_init(&camera, 800, 600, 16));
    ESP_ERROR_CHECK(stream_server_start(&camera));
    esp_err_t audio_result = camera_audio_start();
    if (audio_result != ESP_OK) {
        ESP_LOGW("camera_audio", "Microphone unavailable: %s", esp_err_to_name(audio_result));
    }
    esp_err_t ota_result = camera_ota_confirm_running_app(&camera, audio_result);
    if (ota_result != ESP_OK) {
        ESP_LOGW("camera_ota", "Startup self-test or rollback failed: %s", esp_err_to_name(ota_result));
    }
}
