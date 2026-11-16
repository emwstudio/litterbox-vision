#include <stdlib.h>
#include <string.h>
#include "catvision_direct.h"
#include "catvision_display.h"
#include "live_pipeline.h"
#include "catvision_net.h"
#include "catvision_ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static catvision_image_t *preview;
static lv_obj_t *status;

static void set_status(const char *text)
{
    catvision_display_lock();
    lv_label_set_text(status, text);
    catvision_display_unlock();
}

static void frame_task(void *argument)
{
    (void)argument;
    while (!catvision_net_connected()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    esp_err_t result = catvision_live_start(preview, 466, 466);
    if (result != ESP_OK) {
        set_status("LIVE ERROR");
        ESP_LOGE("live", "start failed: %d", result);
    } else {
        set_status("LIVE");
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(catvision_net_init("catvision-monitor"));
    ESP_ERROR_CHECK(catvision_display_init());
    catvision_display_lock();
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x080c10), LV_PART_MAIN);
    preview = catvision_image_create(screen, 0, 0, 466, 466);
    status = catvision_label(screen, "CONNECTING", 16, 14, 16, 0xffd166);
    catvision_display_unlock();
    catvision_direct_set_overlay(status);
    BaseType_t frames_created = xTaskCreatePinnedToCoreWithCaps(frame_task, "frames", 8192, NULL, 4, NULL, 0,
                                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI("boot", "frames_created=%ld", (long)frames_created);
}
