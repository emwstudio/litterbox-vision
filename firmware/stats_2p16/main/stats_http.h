#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t stats_http_get(const char *url, const char *token, uint8_t **data, size_t *size);
