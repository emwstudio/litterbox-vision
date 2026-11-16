#include "camera_udp_stream.h"

#include <errno.h>
#include <string.h>
#include "camera_udp_control.h"
#include "camera_udp_history.h"
#include "camera_udp_protocol.h"
#include "catvision_net.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static TaskHandle_t udp_task;
static uint32_t udp_sent_frames;
static uint32_t udp_sent_packets;
static uint32_t udp_send_errors;
static uint32_t udp_send_aborts;
static uint32_t udp_retry_successes;
static uint32_t udp_sequence;
static uint32_t udp_capture_sequence;
static uint32_t udp_frame_bytes;
static int udp_last_socket_error;
static uint32_t udp_last_error_sequence;
static uint32_t udp_last_error_index;

void camera_udp_stats(camera_udp_status_t *status)
{
    if (!status) {
        return;
    }
    status->sent_frames = __atomic_load_n(&udp_sent_frames, __ATOMIC_RELAXED);
    status->sent_packets = __atomic_load_n(&udp_sent_packets, __ATOMIC_RELAXED);
    status->send_errors = __atomic_load_n(&udp_send_errors, __ATOMIC_RELAXED);
    status->send_aborts = __atomic_load_n(&udp_send_aborts, __ATOMIC_RELAXED);
    status->retry_successes = __atomic_load_n(&udp_retry_successes, __ATOMIC_RELAXED);
    status->sequence = __atomic_load_n(&udp_sequence, __ATOMIC_RELAXED);
    status->capture_sequence = __atomic_load_n(&udp_capture_sequence, __ATOMIC_RELAXED);
    status->frame_bytes = __atomic_load_n(&udp_frame_bytes, __ATOMIC_RELAXED);
    status->last_socket_error = __atomic_load_n(&udp_last_socket_error, __ATOMIC_RELAXED);
    status->last_error_sequence = __atomic_load_n(&udp_last_error_sequence, __ATOMIC_RELAXED);
    status->last_error_index = __atomic_load_n(&udp_last_error_index, __ATOMIC_RELAXED);
}

esp_err_t camera_udp_send_packet(int socket_fd, const uint8_t *packet, size_t size,
                                 const struct sockaddr_in *destination, uint32_t sequence, uint16_t index)
{
    if (socket_fd < 0 || !packet || !destination) {
        return ESP_ERR_INVALID_ARG;
    }
    for (uint8_t attempt = 0; attempt < 5; attempt++) {
        ssize_t sent = sendto(socket_fd, packet, size, 0,
                              (const struct sockaddr *)destination, sizeof(*destination));
        if (sent >= 0 && (size_t)sent == size) {
            if (attempt > 0) {
                __atomic_add_fetch(&udp_retry_successes, 1, __ATOMIC_RELAXED);
                ESP_LOGW("camera_udp", "recovered seq=%lu index=%u attempt=%u",
                         (unsigned long)sequence, (unsigned int)index, (unsigned int)attempt + 1);
            }
            return ESP_OK;
        }
        int error = sent < 0 ? errno : EIO;
        __atomic_store_n(&udp_last_socket_error, error, __ATOMIC_RELAXED);
        __atomic_store_n(&udp_last_error_sequence, sequence, __ATOMIC_RELAXED);
        __atomic_store_n(&udp_last_error_index, index, __ATOMIC_RELAXED);
        __atomic_add_fetch(&udp_send_errors, 1, __ATOMIC_RELAXED);
        if (sent < 0 && error == ENOMEM && attempt < 4) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        __atomic_add_fetch(&udp_send_aborts, 1, __ATOMIC_RELAXED);
        ESP_LOGW("camera_udp", "abort seq=%lu index=%u error=%d",
                 (unsigned long)sequence, (unsigned int)index, error);
        return error == ENOMEM ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }
    return ESP_FAIL;
}

static void udp_send_loop(void *argument)
{
    camera_board_t *camera = argument;
    struct sockaddr_in destination = {};
    int64_t next_resolution = 0;
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        udp_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    uint8_t *jpeg = heap_caps_malloc(CAMERA_UDP_FRAME_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *packet = heap_caps_malloc(CAMERA_UDP_PACKET_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg || !packet) {
        heap_caps_free(jpeg);
        heap_caps_free(packet);
        close(socket_fd);
        udp_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    uint32_t sequence = 0;
    uint32_t capture_sequence = 0;
    while (true) {
        int64_t now = esp_timer_get_time();
        if (now >= next_resolution) {
            esp_err_t resolution = catvision_resolve_ipv4(CONFIG_CATVISION_UDP_HOST, CONFIG_CATVISION_UDP_PORT, &destination);
            next_resolution = now + (resolution == ESP_OK ? 30000000 : 5000000);
            if (resolution != ESP_OK && destination.sin_family != AF_INET) {
                __atomic_add_fetch(&udp_send_errors, 1, __ATOMIC_RELAXED);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }
        size_t jpeg_size = 0;
        esp_err_t result = camera_board_copy_latest(camera, jpeg, CAMERA_UDP_FRAME_CAPACITY, &capture_sequence, &jpeg_size);
        if (result == ESP_ERR_NOT_FOUND) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (result != ESP_OK || jpeg_size < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8 ||
            jpeg[jpeg_size - 2] != 0xff || jpeg[jpeg_size - 1] != 0xd9) {
            __atomic_add_fetch(&udp_send_errors, 1, __ATOMIC_RELAXED);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        uint32_t crc = camera_udp_crc32(jpeg, jpeg_size);
        uint16_t count = (jpeg_size + CAMERA_UDP_CHUNK_SIZE - 1) / CAMERA_UDP_CHUNK_SIZE;
        for (uint16_t index = 0; index < count; index++) {
            size_t offset = (size_t)index * CAMERA_UDP_CHUNK_SIZE;
            size_t payload_size = jpeg_size - offset < CAMERA_UDP_CHUNK_SIZE ? jpeg_size - offset : CAMERA_UDP_CHUNK_SIZE;
            camera_udp_write_header(packet, CAMERA_UDP_PACKET_SIZE, sequence, index, count, jpeg_size, crc);
            memcpy(packet + CAMERA_UDP_HEADER_SIZE, jpeg + offset, payload_size);
            if (camera_udp_send_packet(socket_fd, packet, CAMERA_UDP_HEADER_SIZE + payload_size,
                                       &destination, sequence, index) != ESP_OK) {
                break;
            }
            __atomic_add_fetch(&udp_sent_packets, 1, __ATOMIC_RELAXED);
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        camera_udp_history_store(sequence, jpeg, jpeg_size, crc);
        __atomic_store_n(&udp_sequence, sequence, __ATOMIC_RELAXED);
        __atomic_store_n(&udp_capture_sequence, capture_sequence, __ATOMIC_RELAXED);
        __atomic_store_n(&udp_frame_bytes, jpeg_size, __ATOMIC_RELAXED);
        uint32_t sent_frames = __atomic_add_fetch(&udp_sent_frames, 1, __ATOMIC_RELAXED);
        sequence++;
        if (sent_frames % 64 == 0) {
            ESP_LOGI("camera_udp", "frames=%lu sequence=%lu bytes=%u packets=%lu errors=%lu",
                     (unsigned long)sent_frames, (unsigned long)capture_sequence,
                     (unsigned int)jpeg_size, (unsigned long)__atomic_load_n(&udp_sent_packets, __ATOMIC_RELAXED),
                     (unsigned long)__atomic_load_n(&udp_send_errors, __ATOMIC_RELAXED));
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

esp_err_t camera_udp_stream_start(camera_board_t *camera)
{
    if (!camera || udp_task) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(CONFIG_CATVISION_UDP_HOST) == 0) {
        ESP_LOGI("camera_udp", "UDP preview disabled");
        return ESP_OK;
    }
    camera_udp_history_init();
    if (xTaskCreatePinnedToCore(udp_send_loop, "camera_udp", 8192, camera, 6, &udp_task, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return camera_udp_control_start();
}
