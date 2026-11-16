#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t udp_receiver_start(void);
esp_err_t udp_receiver_read(const uint8_t **jpeg, size_t *size, uint32_t *seq, uint32_t timeout_ms);
void udp_receiver_release(void);
void udp_receiver_stats(uint32_t *completed, uint32_t *dropped, uint32_t *crc_errors,
                        uint32_t *stale, uint32_t *incomplete, uint32_t *no_slot, uint32_t *queue_full);
