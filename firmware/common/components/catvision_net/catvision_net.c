#include "catvision_net.h"

#include <string.h>
#include <stdlib.h>
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

static EventGroupHandle_t connection_events;
static esp_netif_t *station;

static void network_event(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (connection_events) {
            xEventGroupClearBits(connection_events, BIT0);
        }
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        esp_netif_t *current_default = esp_netif_get_default_netif();
        ESP_LOGI("network", "IP ready: default=%s, station_up=%d",
                 current_default ? esp_netif_get_desc(current_default) : "none",
                 esp_netif_is_netif_up(station));
        if (connection_events) {
            xEventGroupSetBits(connection_events, BIT0);
        }
    }
}

esp_err_t catvision_net_init(const char *hostname)
{
    if (!hostname || hostname[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    connection_events = xEventGroupCreate();
    if (!connection_events) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    station = esp_netif_create_default_wifi_sta();
    if (!station) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_netif_set_hostname(station, hostname));
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(hostname));
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, CONFIG_CATVISION_WIFI_SSID, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, CONFIG_CATVISION_WIFI_PASSWORD, sizeof(config.sta.password));
    config.sta.threshold.authmode = strlen(CONFIG_CATVISION_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    return esp_wifi_connect();
}

bool catvision_net_connected(void)
{
    return connection_events && (xEventGroupGetBits(connection_events) & BIT0);
}

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
    size_t maximum;
} response_buffer_t;

static esp_err_t http_event(esp_http_client_event_t *event)
{
    response_buffer_t *buffer = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_CONNECTED || event->event_id == HTTP_EVENT_HEADERS_SENT) {
        ESP_LOGI("http_debug", "event=%d at_ms=%lld", event->event_id, esp_timer_get_time() / 1000);
        return ESP_OK;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    if (buffer->size == 0) {
        ESP_LOGI("http_debug", "first_data at_ms=%lld bytes=%d", esp_timer_get_time() / 1000, event->data_len);
    }
    if (buffer->size + event->data_len > buffer->maximum) {
        return ESP_ERR_NO_MEM;
    }
    size_t required = buffer->size + event->data_len + 1;
    if (required > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 16384;
        while (capacity < required) {
            capacity *= 2;
        }
        uint8_t *resized = realloc(buffer->data, capacity);
        if (!resized) {
            return ESP_ERR_NO_MEM;
        }
        buffer->data = resized;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->size, event->data, event->data_len);
    buffer->size += event->data_len;
    buffer->data[buffer->size] = 0;
    return ESP_OK;
}

esp_err_t catvision_http_get(const char *url, const char *token, size_t max_bytes, uint8_t **data, size_t *size)
{
    response_buffer_t buffer = {.maximum = max_bytes};
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event,
        .user_data = &buffer,
        .timeout_ms = 10000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }
    if (token && strlen(token)) {
        esp_http_client_set_header(client, "X-Device-Token", token);
    }
    esp_http_client_set_header(client, "Connection", "close");
    int64_t started = esp_timer_get_time();
    esp_err_t result = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI("http_debug", "result=%d status=%d elapsed_ms=%lld", result, status,
             (esp_timer_get_time() - started) / 1000);
    esp_http_client_cleanup(client);
    if (result == ESP_OK && status != 200) {
        result = ESP_ERR_INVALID_RESPONSE;
    }
    if (result != ESP_OK) {
        free(buffer.data);
        return result;
    }
    *data = buffer.data;
    *size = buffer.size;
    return ESP_OK;
}

esp_err_t catvision_http_put(const char *url, const char *token, const char *json)
{
    esp_http_client_config_t config = {.url = url, .timeout_ms = 10000};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }
    esp_http_client_set_method(client, HTTP_METHOD_PUT);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (token && strlen(token)) {
        esp_http_client_set_header(client, "X-Device-Token", token);
    }
    esp_http_client_set_post_field(client, json, strlen(json));
    esp_err_t result = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (result == ESP_OK && status != 200) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return result;
}

void catvision_http_free(uint8_t *data)
{
    free(data);
}
