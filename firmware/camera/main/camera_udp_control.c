#include "camera_udp_control.h"

#include <string.h>
#include "camera_udp_history.h"
#include "camera_udp_protocol.h"
#include "camera_udp_stream.h"
#include "catvision_net.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#define CAMERA_UDP_CONTROL_PORT 5062

static TaskHandle_t control_task;

static void control_loop(void *argument)
{
    (void)argument;
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        control_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(CAMERA_UDP_CONTROL_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = 50000,
    };
    if (bind(socket_fd, (struct sockaddr *)&local, sizeof(local)) != 0 ||
        setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        close(socket_fd);
        control_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    uint8_t *packet = heap_caps_malloc(CAMERA_UDP_PACKET_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!packet) {
        close(socket_fd);
        control_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in hub = {};
    int64_t next_resolution = 0;
    while (true) {
        int64_t now = esp_timer_get_time();
        if (now >= next_resolution) {
            esp_err_t resolution = catvision_resolve_ipv4(CONFIG_CATVISION_UDP_HOST, CAMERA_UDP_CONTROL_PORT, &hub);
            next_resolution = now + (resolution == ESP_OK ? 30000000 : 5000000);
            if (resolution != ESP_OK) {
                continue;
            }
        }
        uint8_t request[CAMERA_UDP_NACK_PACKET_SIZE];
        struct sockaddr_in source = {};
        socklen_t source_size = sizeof(source);
        ssize_t received = recvfrom(socket_fd, request, sizeof(request), 0,
                                    (struct sockaddr *)&source, &source_size);
        if (received < 0) {
            continue;
        }
        if (source.sin_addr.s_addr != hub.sin_addr.s_addr || source.sin_port != htons(5060)) {
            continue;
        }
        camera_udp_repair_cursor_t cursor;
        if (camera_udp_history_begin(request, received, &cursor) != ESP_OK) {
            continue;
        }
        size_t packet_size = 0;
        while (camera_udp_history_next(&cursor, packet, CAMERA_UDP_PACKET_SIZE, &packet_size)) {
            uint16_t index = (uint16_t)(packet[8] | ((uint16_t)packet[9] << 8));
            if (camera_udp_send_packet(socket_fd, packet, packet_size, &source, cursor.sequence, index) != ESP_OK) {
                break;
            }
        }
    }
}

esp_err_t camera_udp_control_start(void)
{
    if (control_task) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xTaskCreatePinnedToCore(control_loop, "camera_udp_control", 8192, NULL, 5, &control_task, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
