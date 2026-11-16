#include "udp_receiver.h"

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "catvision_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define UDP_PORT 5060
#define UDP_HEADER_SIZE 20
#define UDP_PACKET_CAPACITY 1500
#define UDP_CHUNK_SIZE 1400
#define UDP_FRAME_CAPACITY (128 * 1024)
#define UDP_MAX_CHUNKS 128
#define UDP_SLOT_COUNT 2

typedef struct {
    uint8_t *data;
    size_t size;
    uint32_t seq;
} udp_slot_t;

static udp_slot_t slots[UDP_SLOT_COUNT];
static QueueHandle_t free_slots;
static QueueHandle_t ready_slots;
static TaskHandle_t receiver_task;
static udp_slot_t *active_frame;
static volatile uint32_t completed_frames;
static volatile uint32_t dropped_frames;
static volatile uint32_t crc_errors;
static volatile uint32_t stale_frames;
static volatile uint32_t incomplete_frames;
static volatile uint32_t no_slot_frames;
static volatile uint32_t queue_full_frames;

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void release_resources(void)
{
    if (free_slots) {
        vQueueDelete(free_slots);
        free_slots = NULL;
    }
    if (ready_slots) {
        vQueueDelete(ready_slots);
        ready_slots = NULL;
    }
    for (int index = 0; index < UDP_SLOT_COUNT; index++) {
        free(slots[index].data);
        slots[index].data = NULL;
    }
}

static void receiver_loop(void *argument)
{
    (void)argument;
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        ESP_LOGE("udp", "socket failed errno=%d", errno);
        receiver_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 200000};
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(UDP_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_fd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) != 0) {
        ESP_LOGE("udp", "bind failed errno=%d", errno);
        close(socket_fd);
        receiver_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }
    ESP_LOGI("udp", "listening on port %d", UDP_PORT);
    udp_slot_t *current = NULL;
    uint32_t current_seq = 0;
    uint32_t current_size = 0;
    uint32_t current_crc = 0;
    uint16_t current_count = 0;
    uint16_t received = 0;
    uint8_t seen[UDP_MAX_CHUNKS];
    while (true) {
        uint8_t packet[UDP_PACKET_CAPACITY];
        ssize_t size = recv(socket_fd, packet, sizeof(packet), 0);
        if (size < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                ESP_LOGW("udp", "recv failed errno=%d", errno);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            continue;
        }
        if (size < UDP_HEADER_SIZE || memcmp(packet, "CVJ1", 4) != 0) {
            dropped_frames += 1;
            continue;
        }
        uint32_t seq = read_u32(packet + 4);
        uint16_t index = read_u16(packet + 8);
        uint16_t count = read_u16(packet + 10);
        uint32_t frame_size = read_u32(packet + 12);
        uint32_t expected_crc = read_u32(packet + 16);
        size_t payload_size = size - UDP_HEADER_SIZE;
        size_t offset = (size_t)index * UDP_CHUNK_SIZE;
        if (count == 0 || count > UDP_MAX_CHUNKS || index >= count ||
            frame_size == 0 || frame_size > UDP_FRAME_CAPACITY ||
            count != (frame_size + UDP_CHUNK_SIZE - 1) / UDP_CHUNK_SIZE ||
            offset + payload_size > frame_size ||
            (index + 1 < count && payload_size != UDP_CHUNK_SIZE) ||
            (index + 1 == count && offset + payload_size != frame_size)) {
            dropped_frames += 1;
            continue;
        }
        if (current && seq == 0 && index == 0) {
            xQueueSend(free_slots, &current, 0);
            current = NULL;
        }
        if (current && (int32_t)(seq - current_seq) < 0) {
            dropped_frames += 1;
            stale_frames += 1;
            continue;
        }
        if (current && seq != current_seq) {
            dropped_frames += 1;
            incomplete_frames += 1;
            xQueueSend(free_slots, &current, 0);
            current = NULL;
        }
        if (!current) {
            if (xQueueReceive(free_slots, &current, 0) != pdPASS) {
                dropped_frames += 1;
                no_slot_frames += 1;
                continue;
            }
            current_seq = seq;
            current_size = frame_size;
            current_crc = expected_crc;
            current_count = count;
            received = 0;
            memset(seen, 0, sizeof(seen));
        }
        if (current_size != frame_size || current_crc != expected_crc || current_count != count) {
            dropped_frames += 1;
            xQueueSend(free_slots, &current, 0);
            current = NULL;
            continue;
        }
        uint8_t mask = 1U << (index & 7);
        if (seen[index >> 3] & mask) {
            continue;
        }
        memcpy(current->data + offset, packet + UDP_HEADER_SIZE, payload_size);
        seen[index >> 3] |= mask;
        received++;
        if (received != current_count) {
            continue;
        }
        if (catvision_crc32(current->data, current_size) != expected_crc) {
            crc_errors += 1;
            xQueueSend(free_slots, &current, 0);
            current = NULL;
            continue;
        }
        current->size = current_size;
        current->seq = current_seq;
        completed_frames += 1;
        if (xQueueSend(ready_slots, &current, 0) != pdPASS) {
            dropped_frames += 1;
            queue_full_frames += 1;
            xQueueSend(free_slots, &current, 0);
        }
        current = NULL;
    }
}

esp_err_t udp_receiver_start(void)
{
    if (receiver_task) {
        return ESP_ERR_INVALID_STATE;
    }
    free_slots = xQueueCreate(UDP_SLOT_COUNT, sizeof(udp_slot_t *));
    ready_slots = xQueueCreate(UDP_SLOT_COUNT, sizeof(udp_slot_t *));
    if (!free_slots || !ready_slots) {
        release_resources();
        return ESP_ERR_NO_MEM;
    }
    for (int index = 0; index < UDP_SLOT_COUNT; index++) {
        slots[index].data = static_cast<uint8_t *>(heap_caps_malloc(UDP_FRAME_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!slots[index].data) {
            release_resources();
            return ESP_ERR_NO_MEM;
        }
        udp_slot_t *slot = &slots[index];
        xQueueSend(free_slots, &slot, 0);
    }
    if (xTaskCreatePinnedToCoreWithCaps(receiver_loop, "udp_receiver", 8192, NULL, 6, &receiver_task, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        release_resources();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t udp_receiver_read(const uint8_t **jpeg, size_t *size, uint32_t *seq, uint32_t timeout_ms)
{
    if (!jpeg || !size || !seq || !ready_slots || active_frame) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xQueueReceive(ready_slots, &active_frame, pdMS_TO_TICKS(timeout_ms)) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    *jpeg = active_frame->data;
    *size = active_frame->size;
    *seq = active_frame->seq;
    return ESP_OK;
}

void udp_receiver_release(void)
{
    if (active_frame) {
        xQueueSend(free_slots, &active_frame, 0);
        active_frame = NULL;
    }
}

void udp_receiver_stats(uint32_t *completed, uint32_t *dropped, uint32_t *crc_errors_out,
                        uint32_t *stale, uint32_t *incomplete, uint32_t *no_slot, uint32_t *queue_full)
{
    if (completed) {
        *completed = completed_frames;
    }
    if (dropped) {
        *dropped = dropped_frames;
    }
    if (crc_errors_out) {
        *crc_errors_out = crc_errors;
    }
    if (stale) {
        *stale = stale_frames;
    }
    if (incomplete) {
        *incomplete = incomplete_frames;
    }
    if (no_slot) {
        *no_slot = no_slot_frames;
    }
    if (queue_full) {
        *queue_full = queue_full_frames;
    }
}
