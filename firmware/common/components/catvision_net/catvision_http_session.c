#include "catvision_http_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static esp_err_t session_event(esp_http_client_event_t *event)
{
    catvision_http_session_t *session = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_CONNECTED) {
        session->connected_us = esp_timer_get_time();
    } else if (event->event_id == HTTP_EVENT_HEADERS_SENT) {
        session->headers_sent_us = esp_timer_get_time();
    } else if (event->event_id == HTTP_EVENT_ON_HEADER) {
        ESP_LOGI("http_header", "%s: %s", event->header_key ?: "-", event->header_value ?: "-");
        if (strcasecmp(event->header_key, "X-CatVision-Delta") == 0) {
            session->delta = strcmp(event->header_value, "1") == 0;
        } else if (strcasecmp(event->header_key, "X-CatVision-Frame-CRC") == 0) {
            session->frame_crc = (uint32_t)strtoul(event->header_value, NULL, 10);
            session->has_frame_crc = true;
        }
    }
    return ESP_OK;
}

esp_err_t catvision_http_session_open(catvision_http_session_t *session, const char *url, const char *token, size_t max_bytes)
{
    memset(session, 0, sizeof(*session));
    session->maximum = max_bytes;
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = session_event,
        .user_data = session,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .keep_alive_enable = false,
    };
    session->client = esp_http_client_init(&config);
    if (!session->client) {
        return ESP_FAIL;
    }
    if (token && strlen(token)) {
        esp_http_client_set_header(session->client, "X-Device-Token", token);
    }
    esp_http_client_set_header(session->client, "Connection", "close");
    return ESP_OK;
}

esp_err_t catvision_http_session_set_frame_crc(catvision_http_session_t *session, uint32_t crc)
{
    if (!session || !session->client) {
        return ESP_ERR_INVALID_ARG;
    }
    char value[16];
    snprintf(value, sizeof(value), "%lu", (unsigned long)crc);
    return esp_http_client_set_header(session->client, "X-CatVision-Frame-CRC", value);
}

esp_err_t catvision_http_session_get(catvision_http_session_t *session, uint8_t **data, size_t *size)
{
    if (!session || !session->client || !data || !size) {
        return ESP_ERR_INVALID_ARG;
    }
    session->size = 0;
    session->delta = false;
    session->has_frame_crc = false;
    session->frame_crc = 0;
    session->request_started_us = esp_timer_get_time();
    session->connected_us = 0;
    session->headers_sent_us = 0;
    session->first_data_us = 0;
    session->data_events = 0;
    esp_err_t result = esp_http_client_open(session->client, 0);
    int64_t content_length = result == ESP_OK ? esp_http_client_fetch_headers(session->client) : -1;
    int status = esp_http_client_get_status_code(session->client);
    if (result == ESP_OK && content_length < 0) {
        result = ESP_ERR_INVALID_RESPONSE;
    }
    if (result == ESP_OK && status != 200) {
        result = ESP_ERR_INVALID_RESPONSE;
    }
    if (result == ESP_OK && content_length > (int64_t)session->maximum) {
        result = ESP_ERR_NO_MEM;
    }
    if (result == ESP_OK) {
        size_t required = (size_t)content_length + 1;
        if (required > session->capacity) {
            uint8_t *resized = realloc(session->data, required);
            if (!resized) {
                result = ESP_ERR_NO_MEM;
            } else {
                session->data = resized;
                session->capacity = required;
            }
        }
    }
    while (result == ESP_OK && session->size < (size_t)content_length) {
        int read = esp_http_client_read(session->client, (char *)session->data + session->size,
                                       (size_t)content_length - session->size);
        if (read <= 0) {
            result = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (session->data_events++ == 0) {
            session->first_data_us = esp_timer_get_time();
        }
        session->size += read;
    }
    if (result == ESP_OK && !esp_http_client_is_complete_data_received(session->client)) {
        result = ESP_ERR_INVALID_RESPONSE;
    }
    int rssi = 0;
    wifi_ps_type_t power_save = WIFI_PS_NONE;
    esp_wifi_sta_get_rssi(&rssi);
    esp_wifi_get_ps(&power_save);
    ESP_LOGI("http_profile", "result=%d status=%d content_length=%lld rssi=%d ps=%d connect_ms=%lld request_ms=%lld first_byte_ms=%lld total_ms=%lld chunks=%u",
             result, status, (long long)content_length, rssi, power_save,
             (session->connected_us - session->request_started_us) / 1000,
             (session->headers_sent_us - session->request_started_us) / 1000,
             (session->first_data_us - session->request_started_us) / 1000,
             (esp_timer_get_time() - session->request_started_us) / 1000, session->data_events);
    if (result != ESP_OK) {
        return result;
    }
    session->data[session->size] = 0;
    *data = session->data;
    *size = session->size;
    return ESP_OK;
}

void catvision_http_session_close(catvision_http_session_t *session)
{
    if (!session) {
        return;
    }
    if (session->client) {
        esp_http_client_cleanup(session->client);
    }
    free(session->data);
    memset(session, 0, sizeof(*session));
}
