#include <stdlib.h>
#include <string.h>
#include "catvision_direct.h"
#include "catvision_display.h"
#include "stats_http.h"
#include "live_pipeline.h"
#include "catvision_net.h"
#include "catvision_ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "stats_ui.h"

static lv_obj_t *stats_status;
static lv_obj_t *live_status;

static void set_status(lv_obj_t *label, const char *text)
{
    catvision_display_lock();
    lv_label_set_text(label, text);
    catvision_display_unlock();
}

static void frame_task(void *argument)
{
    (void)argument;
    while (!catvision_net_connected()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    esp_err_t result = catvision_live_start(stats_ui_live_image(), 480, 240);
    if (result != ESP_OK) {
        set_status(live_status, "LIVE ERROR");
        ESP_LOGE("live", "start failed: %d", result);
    } else {
        set_status(live_status, "LIVE");
    }
    vTaskDelete(NULL);
}

static void stats_task(void *argument)
{
    stats_ui_load_defaults();
    while (!catvision_net_connected()) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    while (true) {
        uint8_t *data = NULL;
        size_t size = 0;
        char url[256];
        snprintf(url, sizeof(url), "%s/api/display/stats.json", CONFIG_CATVISION_SERVER_URL);
        esp_err_t result = stats_http_get(url, CONFIG_CATVISION_DEVICE_TOKEN, &data, &size);
        if (result == ESP_OK) {
            stats_ui_update((const char *)data);
            catvision_http_free(data);
        } else {
            ESP_LOGW("stats", "request result=%d", result);
            set_status(stats_status, "STATS OFFLINE");
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(catvision_net_init("catvision-stats"));
    ESP_ERROR_CHECK(catvision_display_init());
    catvision_display_lock();
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x080c10), LV_PART_MAIN);
    stats_status = catvision_label(screen, "WAITING FOR CATS", 16, 16, 16, 0xaab4c0);
    lv_obj_set_style_bg_opa(stats_status, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(stats_status, LV_OBJ_FLAG_HIDDEN);
    live_status = catvision_label(screen, "CONNECTING", 414, 16, 16, 0xffd166);
    lv_obj_set_style_bg_opa(live_status, LV_OPA_TRANSP, LV_PART_MAIN);
    ESP_ERROR_CHECK(stats_ui_init(screen, stats_status));
    lv_obj_move_foreground(live_status);
    catvision_display_unlock();
    catvision_direct_set_overlay(live_status);
    BaseType_t frames_created = xTaskCreatePinnedToCoreWithCaps(frame_task, "frames", 16384, NULL, 4, NULL, 0,
                                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    BaseType_t stats_created = xTaskCreatePinnedToCoreWithCaps(stats_task, "stats", 6144, NULL, 3, NULL, 0,
                                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI("boot", "frames_created=%ld stats_created=%ld", (long)frames_created, (long)stats_created);
}
