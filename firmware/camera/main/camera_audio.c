#include "camera_audio.h"

#include <string.h>
#include "bsp/esp32_s3_cam_ovxxxx.h"
#include "camera_audio_protocol.h"
#include "catvision_net.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#define CAMERA_AUDIO_RAW_BYTES (CAMERA_AUDIO_SAMPLE_COUNT * 2 * 4)

static esp_codec_dev_handle_t microphone;
static const audio_codec_ctrl_if_t *microphone_control;
static const audio_codec_data_if_t *microphone_data;
static const audio_codec_if_t *microphone_codec;
static TaskHandle_t audio_task;
static uint8_t *raw_buffer;
static uint8_t *pcm_buffer;
static uint8_t *packet_buffer;
static int audio_socket = -1;
static uint32_t packets_sent;
static uint32_t audio_errors;
static int64_t last_packet_us;
static struct sockaddr_in audio_destination;

static void release_audio(void)
{
    if (microphone) {
        esp_codec_dev_close(microphone);
        esp_codec_dev_delete(microphone);
        microphone = NULL;
    }
    if (microphone_codec) {
        audio_codec_delete_codec_if(microphone_codec);
        microphone_codec = NULL;
    }
    if (microphone_control) {
        audio_codec_delete_ctrl_if(microphone_control);
        microphone_control = NULL;
    }
    if (microphone_data) {
        audio_codec_delete_data_if(microphone_data);
        microphone_data = NULL;
    }
    heap_caps_free(raw_buffer);
    heap_caps_free(pcm_buffer);
    heap_caps_free(packet_buffer);
    raw_buffer = NULL;
    pcm_buffer = NULL;
    packet_buffer = NULL;
    if (audio_socket >= 0) {
        close(audio_socket);
        audio_socket = -1;
    }
}

static esp_err_t open_microphone(void)
{
    const audio_codec_data_if_t *data = bsp_audio_get_codec_itf();
    if (!data) {
        esp_err_t result = bsp_audio_init(NULL);
        if (result != ESP_OK) {
            return result;
        }
        data = bsp_audio_get_codec_itf();
    }
    if (!data) {
        return ESP_FAIL;
    }
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    uint8_t address = 0;
    esp_err_t result = ESP_ERR_NOT_FOUND;
    for (uint8_t candidate = 0x80; candidate <= 0x86; candidate += 2) {
        result = i2c_master_probe(bus, candidate >> 1, 100);
        ESP_LOGI("camera_audio", "ES7210 probe 0x%02x: %s", candidate >> 1, esp_err_to_name(result));
        if (result == ESP_OK) {
            address = candidate;
            break;
        }
    }
    if (address == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    audio_codec_i2c_cfg_t i2c_config = {
        .port = CONFIG_BSP_I2C_NUM,
        .addr = address,
        .bus_handle = bus,
    };
    microphone_control = audio_codec_new_i2c_ctrl(&i2c_config);
    if (!microphone_control) {
        return ESP_ERR_NO_MEM;
    }
    microphone_data = data;
    es7210_codec_cfg_t codec_config = {
        .ctrl_if = microphone_control,
    };
    microphone_codec = es7210_codec_new(&codec_config);
    if (!microphone_codec) {
        return ESP_FAIL;
    }
    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = microphone_codec,
        .data_if = microphone_data,
    };
    microphone = esp_codec_dev_new(&device_config);
    return microphone ? ESP_OK : ESP_FAIL;
}

static void audio_loop(void *argument)
{
    (void)argument;
    struct sockaddr_in destination = audio_destination;
    int64_t next_resolution = esp_timer_get_time() + 30000000;
    uint32_t sequence = 0;
    uint64_t sample_index = 0;
    while (true) {
        int64_t now = esp_timer_get_time();
        if (now >= next_resolution) {
            esp_err_t resolution = catvision_resolve_ipv4(CONFIG_CATVISION_UDP_HOST, CONFIG_CATVISION_AUDIO_UDP_PORT, &destination);
            next_resolution = now + (resolution == ESP_OK ? 30000000 : 5000000);
        }
        esp_err_t result = esp_codec_dev_read(microphone, raw_buffer, CAMERA_AUDIO_RAW_BYTES);
        if (result != ESP_OK) {
            uint32_t errors = __atomic_add_fetch(&audio_errors, 1, __ATOMIC_RELAXED);
            if (errors % 32 == 1) {
                ESP_LOGW("camera_audio", "microphone read failed: %s errors=%lu", esp_err_to_name(result), (unsigned long)errors);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (camera_audio_pcm_from_stereo32((const int32_t *)raw_buffer, CAMERA_AUDIO_SAMPLE_COUNT,
                                           pcm_buffer, CAMERA_AUDIO_PCM_BYTES, 0) != CAMERA_AUDIO_PCM_BYTES) {
            __atomic_add_fetch(&audio_errors, 1, __ATOMIC_RELAXED);
            continue;
        }
        size_t size = camera_audio_write_packet(packet_buffer, CAMERA_AUDIO_PACKET_BYTES, sequence, sample_index,
                                                pcm_buffer, CAMERA_AUDIO_PCM_BYTES);
        ssize_t sent = size ? sendto(audio_socket, packet_buffer, size, 0,
                                     (struct sockaddr *)&destination, sizeof(destination)) : -1;
        if (sent < 0 || (size_t)sent != size) {
            uint32_t errors = __atomic_add_fetch(&audio_errors, 1, __ATOMIC_RELAXED);
            if (errors % 32 == 1) {
                ESP_LOGW("camera_audio", "audio send failed errors=%lu", (unsigned long)errors);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        __atomic_add_fetch(&packets_sent, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&last_packet_us, esp_timer_get_time(), __ATOMIC_RELAXED);
        sequence++;
        sample_index += CAMERA_AUDIO_SAMPLE_COUNT;
        if (sequence % 256 == 0) {
            uint32_t peak = 0;
            for (size_t index = 0; index < CAMERA_AUDIO_SAMPLE_COUNT; index++) {
                int16_t sample;
                memcpy(&sample, pcm_buffer + index * 2, sizeof(sample));
                uint32_t amplitude = sample < 0 ? (uint32_t)(-(int32_t)sample) : (uint32_t)sample;
                if (amplitude > peak) {
                    peak = amplitude;
                }
            }
            ESP_LOGI("camera_audio", "packets=%lu sample=%llu errors=%lu peak=%lu",
                     (unsigned long)packets_sent, (unsigned long long)sample_index,
                     (unsigned long)audio_errors, (unsigned long)peak);
        }
    }
}

esp_err_t camera_audio_start(void)
{
    if (!CONFIG_CATVISION_AUDIO_ENABLED || strlen(CONFIG_CATVISION_UDP_HOST) == 0) {
        return ESP_OK;
    }
    if (microphone || audio_task) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = catvision_resolve_ipv4(CONFIG_CATVISION_UDP_HOST, CONFIG_CATVISION_AUDIO_UDP_PORT, &audio_destination);
    if (result == ESP_OK) {
        result = open_microphone();
    }
    if (result == ESP_OK) {
        result = esp_codec_dev_set_in_gain(microphone, 42.0f);
    }
    esp_codec_dev_sample_info_t format = {
        .bits_per_sample = 32,
        .channel = 2,
        .sample_rate = CAMERA_AUDIO_SAMPLE_RATE,
    };
    if (result == ESP_OK) {
        result = esp_codec_dev_open(microphone, &format);
    }
    if (result == ESP_OK) {
        raw_buffer = heap_caps_malloc(CAMERA_AUDIO_RAW_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        pcm_buffer = heap_caps_malloc(CAMERA_AUDIO_PCM_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        packet_buffer = heap_caps_malloc(CAMERA_AUDIO_PACKET_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!raw_buffer || !pcm_buffer || !packet_buffer) {
            result = ESP_ERR_NO_MEM;
        }
    }
    if (result == ESP_OK) {
        audio_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (audio_socket < 0) {
            result = ESP_FAIL;
        }
    }
    if (result == ESP_OK &&
        xTaskCreatePinnedToCore(audio_loop, "camera_audio", 8192, NULL, 4, &audio_task, 0) != pdPASS) {
        result = ESP_ERR_NO_MEM;
    }
    if (result != ESP_OK) {
        release_audio();
        return result;
    }
    ESP_LOGI("camera_audio", "ES7210 ready rate=%d udp_port=%d", CAMERA_AUDIO_SAMPLE_RATE, CONFIG_CATVISION_AUDIO_UDP_PORT);
    return ESP_OK;
}

void camera_audio_stats(uint32_t *packets, uint32_t *errors, int64_t *last_packet_us_out)
{
    if (packets) {
        *packets = __atomic_load_n(&packets_sent, __ATOMIC_RELAXED);
    }
    if (errors) {
        *errors = __atomic_load_n(&audio_errors, __ATOMIC_RELAXED);
    }
    if (last_packet_us_out) {
        *last_packet_us_out = __atomic_load_n(&last_packet_us, __ATOMIC_RELAXED);
    }
}
