#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

struct sockaddr_in;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t catvision_net_init(const char *hostname);
bool catvision_net_connected(void);
esp_err_t catvision_resolve_ipv4(const char *host, int port, struct sockaddr_in *destination);
esp_err_t catvision_http_get(const char *url, const char *token, size_t max_bytes, uint8_t **data, size_t *size);
esp_err_t catvision_http_put(const char *url, const char *token, const char *json);
void catvision_http_free(uint8_t *data);

#ifdef __cplusplus
}
#endif
