#include "camera_udp_history.h"

#include <string.h>
#include "camera_udp_protocol.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    uint32_t sequence;
    uint32_t frame_size;
    uint32_t crc;
    uint8_t *jpeg;
} camera_udp_history_entry_t;

static SemaphoreHandle_t history_mutex;
static camera_udp_history_entry_t entries[CAMERA_UDP_HISTORY_MAX_FRAMES];
static uint32_t entry_count;
static uint32_t entry_bytes;
static camera_udp_history_stats_t history_stats;

static uint16_t read_u16(const uint8_t *input)
{
    return (uint16_t)(input[0] | ((uint16_t)input[1] << 8));
}

static uint32_t read_u32(const uint8_t *input)
{
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8) |
           ((uint32_t)input[2] << 16) | ((uint32_t)input[3] << 24);
}

static camera_udp_history_entry_t *find_entry(uint32_t sequence)
{
    for (uint32_t index = 0; index < entry_count; index++) {
        if (entries[index].sequence == sequence) {
            return &entries[index];
        }
    }
    return NULL;
}

void camera_udp_history_init(void)
{
    if (!history_mutex) {
        history_mutex = xSemaphoreCreateMutex();
    }
    if (!history_mutex) {
        return;
    }
    xSemaphoreTake(history_mutex, portMAX_DELAY);
    for (uint32_t index = 0; index < entry_count; index++) {
        heap_caps_free(entries[index].jpeg);
    }
    memset(entries, 0, sizeof(entries));
    entry_count = 0;
    entry_bytes = 0;
    memset(&history_stats, 0, sizeof(history_stats));
    xSemaphoreGive(history_mutex);
}

esp_err_t camera_udp_history_store(uint32_t sequence, const uint8_t *jpeg, uint32_t frame_size, uint32_t crc)
{
    if (!jpeg || frame_size == 0 || frame_size > CAMERA_UDP_FRAME_CAPACITY) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!history_mutex) {
        camera_udp_history_init();
    }
    if (!history_mutex) {
        return ESP_ERR_NO_MEM;
    }
    uint8_t *copy = heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        xSemaphoreTake(history_mutex, portMAX_DELAY);
        history_stats.drops++;
        xSemaphoreGive(history_mutex);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, jpeg, frame_size);
    xSemaphoreTake(history_mutex, portMAX_DELAY);
    while (entry_count > 0 && (entry_count >= CAMERA_UDP_HISTORY_MAX_FRAMES || entry_bytes + frame_size > CAMERA_UDP_HISTORY_MAX_BYTES)) {
        entry_bytes -= entries[0].frame_size;
        heap_caps_free(entries[0].jpeg);
        memmove(&entries[0], &entries[1], (entry_count - 1) * sizeof(entries[0]));
        entry_count--;
    }
    entries[entry_count++] = (camera_udp_history_entry_t){
        .sequence = sequence,
        .frame_size = frame_size,
        .crc = crc,
        .jpeg = copy,
    };
    entry_bytes += frame_size;
    xSemaphoreGive(history_mutex);
    return ESP_OK;
}

esp_err_t camera_udp_history_begin(const uint8_t *request, size_t request_size, camera_udp_repair_cursor_t *cursor)
{
    if (!request || !cursor || request_size != CAMERA_UDP_NACK_PACKET_SIZE || memcmp(request, "CVN1", 4) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t sequence = read_u32(request + 4);
    uint16_t first_index = read_u16(request + 8);
    uint16_t index_count = read_u16(request + 10);
    const uint8_t *mask = request + 12;
    if (first_index >= 128 || index_count > 128 || first_index + index_count > 128) {
        return ESP_ERR_INVALID_ARG;
    }
    if (index_count == 0) {
        if (first_index != 0) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t index = 0; index < 16; index++) {
            if (mask[index] != 0) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    } else {
        bool any = false;
        for (uint16_t index = 0; index < 128; index++) {
            bool set = (mask[index >> 3] & (1 << (index & 7))) != 0;
            if (index < index_count && set) {
                any = true;
            }
            if (index >= index_count && set) {
                return ESP_ERR_INVALID_ARG;
            }
        }
        if (!any) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (!history_mutex) {
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(history_mutex, portMAX_DELAY);
    camera_udp_history_entry_t *entry = find_entry(sequence);
    if (!entry) {
        xSemaphoreGive(history_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    uint16_t available = (entry->frame_size + CAMERA_UDP_CHUNK_SIZE - 1) / CAMERA_UDP_CHUNK_SIZE;
    if (index_count > 0 && first_index + index_count > available) {
        xSemaphoreGive(history_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    *cursor = (camera_udp_repair_cursor_t){
        .sequence = sequence,
        .first_index = first_index,
        .index_count = index_count == 0 ? available : index_count,
        .next_index = first_index,
    };
    if (index_count == 0) {
        memset(cursor->mask, 0xff, sizeof(cursor->mask));
    } else {
        memcpy(cursor->mask, mask, sizeof(cursor->mask));
    }
    history_stats.nack_requests++;
    xSemaphoreGive(history_mutex);
    return ESP_OK;
}

bool camera_udp_history_next(camera_udp_repair_cursor_t *cursor, uint8_t *packet, size_t capacity, size_t *packet_size)
{
    if (!cursor || !packet || !packet_size || !history_mutex) {
        return false;
    }
    *packet_size = 0;
    xSemaphoreTake(history_mutex, portMAX_DELAY);
    bool found = false;
    camera_udp_history_entry_t *entry = find_entry(cursor->sequence);
    if (entry) {
        uint16_t end = cursor->first_index + cursor->index_count;
        while (cursor->next_index < end) {
            uint16_t index = cursor->next_index++;
            uint16_t relative = index - cursor->first_index;
            if ((cursor->mask[relative >> 3] & (1 << (relative & 7))) == 0) {
                continue;
            }
            size_t offset = (size_t)index * CAMERA_UDP_CHUNK_SIZE;
            if (offset >= entry->frame_size) {
                break;
            }
            size_t payload_size = entry->frame_size - offset;
            if (payload_size > CAMERA_UDP_CHUNK_SIZE) {
                payload_size = CAMERA_UDP_CHUNK_SIZE;
            }
            if (capacity < CAMERA_UDP_HEADER_SIZE + payload_size) {
                break;
            }
            camera_udp_write_header(packet, capacity, cursor->sequence, index,
                                    (entry->frame_size + CAMERA_UDP_CHUNK_SIZE - 1) / CAMERA_UDP_CHUNK_SIZE,
                                    entry->frame_size, entry->crc);
            memcpy(packet + CAMERA_UDP_HEADER_SIZE, entry->jpeg + offset, payload_size);
            *packet_size = CAMERA_UDP_HEADER_SIZE + payload_size;
            history_stats.retransmitted_packets++;
            found = true;
            break;
        }
    }
    xSemaphoreGive(history_mutex);
    return found;
}

void camera_udp_history_stats(camera_udp_history_stats_t *stats)
{
    if (!stats) {
        return;
    }
    if (history_mutex) {
        xSemaphoreTake(history_mutex, portMAX_DELAY);
        *stats = history_stats;
        stats->frames = entry_count;
        stats->bytes = entry_bytes;
        xSemaphoreGive(history_mutex);
    } else {
        *stats = history_stats;
        stats->frames = 0;
        stats->bytes = 0;
    }
}
