#include "catvision_present.h"

#include <stdlib.h>
#include "esp_heap_caps.h"
#ifdef ESP_PLATFORM
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#endif

#define PRESENT_SLOTS 2

typedef enum {
    PRESENT_FREE,
    PRESENT_QUEUED,
    PRESENT_RUNNING,
    PRESENT_DONE,
} present_state_t;

typedef struct {
    catvision_image_t *image;
    uint8_t *data;
    size_t size;
    uint32_t crc;
    int width;
    int height;
    esp_err_t result;
    volatile present_state_t state;
} present_job_t;

static present_job_t present_jobs[PRESENT_SLOTS];
static volatile uint8_t present_enqueue;
static volatile uint8_t present_dequeue;
static int present_outstanding;
static SemaphoreHandle_t present_free_slots;
static SemaphoreHandle_t present_done_slots;
static SemaphoreHandle_t present_work_slots;
#ifdef ESP_PLATFORM
static TaskHandle_t present_task;
#endif

static void present_run(uint8_t index)
{
    present_job_t *job = &present_jobs[index];
    job->state = PRESENT_RUNNING;
    job->result = catvision_direct_update_owned_native(job->image, job->data, job->size, job->crc, job->width, job->height);
    job->state = PRESENT_DONE;
    xSemaphoreGive(present_done_slots);
}

#ifdef ESP_PLATFORM
static void present_worker(void *argument)
{
    (void)argument;
    while (true) {
        xSemaphoreTake(present_work_slots, portMAX_DELAY);
        uint8_t index = present_dequeue;
        present_dequeue = (present_dequeue + 1) % PRESENT_SLOTS;
        present_run(index);
    }
}
#else
static bool present_run_next(void)
{
    if (xSemaphoreTake(present_work_slots, 0) != pdPASS) {
        return false;
    }
    uint8_t index = present_dequeue;
    present_dequeue = (present_dequeue + 1) % PRESENT_SLOTS;
    present_run(index);
    return true;
}
#endif

static esp_err_t present_reap(void)
{
    for (int index = 0; index < PRESENT_SLOTS; index++) {
        present_job_t *job = &present_jobs[index];
        if (job->state != PRESENT_DONE) {
            continue;
        }
        esp_err_t result = job->result;
        if (result != ESP_OK) {
            if (job->image->delta_base == job->data) {
                job->image->delta_base = job->image->pixels;
            }
            free(job->data);
        }
        job->data = NULL;
        job->state = PRESENT_FREE;
        present_outstanding--;
        xSemaphoreGive(present_free_slots);
        return result;
    }
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t present_poll(void)
{
    esp_err_t first_error = ESP_OK;
    while (xSemaphoreTake(present_done_slots, 0) == pdPASS) {
        esp_err_t result = present_reap();
        if (first_error == ESP_OK && result != ESP_OK) {
            first_error = result;
        }
    }
    return first_error;
}

esp_err_t catvision_present_init(void)
{
    present_free_slots = xSemaphoreCreateCounting(PRESENT_SLOTS, PRESENT_SLOTS);
    present_done_slots = xSemaphoreCreateCounting(PRESENT_SLOTS, 0);
    present_work_slots = xSemaphoreCreateCounting(PRESENT_SLOTS, 0);
    if (!present_free_slots || !present_done_slots || !present_work_slots) {
        if (present_free_slots) {
            vSemaphoreDelete(present_free_slots);
            present_free_slots = NULL;
        }
        if (present_done_slots) {
            vSemaphoreDelete(present_done_slots);
            present_done_slots = NULL;
        }
        if (present_work_slots) {
            vSemaphoreDelete(present_work_slots);
            present_work_slots = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
#ifdef ESP_PLATFORM
    if (xTaskCreatePinnedToCoreWithCaps(present_worker, "live_present", 8192, NULL, 4, &present_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vSemaphoreDelete(present_free_slots);
        vSemaphoreDelete(present_done_slots);
        vSemaphoreDelete(present_work_slots);
        present_free_slots = NULL;
        present_done_slots = NULL;
        present_work_slots = NULL;
        return ESP_ERR_NO_MEM;
    }
#endif
    return ESP_OK;
}

esp_err_t catvision_present_wait(void)
{
    esp_err_t first_error = present_poll();
    while (present_outstanding > 0) {
#ifdef ESP_PLATFORM
        if (xSemaphoreTake(present_done_slots, pdMS_TO_TICKS(2000)) != pdPASS) {
            return ESP_ERR_TIMEOUT;
        }
        esp_err_t result = present_reap();
#else
        if (!present_run_next()) {
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t result = present_poll();
#endif
        if (first_error == ESP_OK && result != ESP_OK) {
            first_error = result;
        }
    }
    return first_error;
}

static bool present_owns(const uint8_t *data)
{
    for (int index = 0; index < PRESENT_SLOTS; index++) {
        if (present_jobs[index].state != PRESENT_FREE && present_jobs[index].data == data) {
            return true;
        }
    }
    return false;
}

esp_err_t catvision_direct_abort_pending(catvision_image_t *image)
{
    if (!image) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = catvision_present_wait();
    image->delta_base = image->pixels;
    return result;
}

esp_err_t catvision_direct_commit_decoded(catvision_image_t *image, uint8_t *data)
{
    if (!image || !data) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = present_poll();
    if (result != ESP_OK) {
        return result;
    }
    if (image->delta_base == data) {
        return ESP_OK;
    }
    uint8_t *previous = image->delta_base;
    image->delta_base = data;
    if (previous && previous != image->pixels && !present_owns(previous)) {
        free(previous);
    }
    return ESP_OK;
}

esp_err_t catvision_direct_update_owned_native_async(catvision_image_t *image, uint8_t *data, size_t size, uint32_t expected_crc, int width, int height)
{
    if (!image || !data || width <= 0 || height <= 0 || size != (size_t)width * height * 2) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = present_poll();
    if (result != ESP_OK) {
        return result;
    }
    while (xSemaphoreTake(present_free_slots, 0) != pdPASS) {
#ifdef ESP_PLATFORM
        if (xSemaphoreTake(present_done_slots, pdMS_TO_TICKS(2000)) != pdPASS) {
            return ESP_ERR_TIMEOUT;
        }
        result = present_reap();
#else
        if (!present_run_next()) {
            return ESP_ERR_INVALID_STATE;
        }
        result = present_poll();
#endif
        if (result != ESP_OK) {
            return result;
        }
    }
    uint8_t index = present_enqueue;
    present_job_t *job = &present_jobs[index];
    if (job->state != PRESENT_FREE) {
        xSemaphoreGive(present_free_slots);
        return ESP_ERR_INVALID_STATE;
    }
    job->image = image;
    job->data = data;
    job->size = size;
    job->crc = expected_crc;
    job->width = width;
    job->height = height;
    job->result = ESP_ERR_INVALID_STATE;
    job->state = PRESENT_QUEUED;
    present_enqueue = (present_enqueue + 1) % PRESENT_SLOTS;
    present_outstanding++;
    xSemaphoreGive(present_work_slots);
#ifdef ESP_PLATFORM
    if (!present_task) {
        present_outstanding--;
        job->state = PRESENT_FREE;
        xSemaphoreGive(present_free_slots);
        return ESP_ERR_INVALID_STATE;
    }
    xTaskNotifyGive(present_task);
#endif
    return ESP_OK;
}
