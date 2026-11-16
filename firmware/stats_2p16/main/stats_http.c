#include "stats_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "catvision_net.h"
#include "lwip/sockets.h"

#define RESPONSE_CAPACITY (512 * 1024)

esp_err_t stats_http_get(const char *url, const char *token, uint8_t **data, size_t *size)
{
    if (!url || !token || !data || !size) {
        return ESP_ERR_INVALID_ARG;
    }
    char host[64];
    char path[256];
    int port = 0;
    if (sscanf(url, "http://%63[^:]:%d/%255[^\n]", host, &port, path) != 3) {
        return ESP_ERR_INVALID_ARG;
    }
    int socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd < 0) {
        return ESP_FAIL;
    }
    struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in address = {};
    esp_err_t result = catvision_resolve_ipv4(host, port, &address);
    if (result != ESP_OK) {
        close(socket_fd);
        return result;
    }
    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        ESP_LOGW("stats_http", "connect failed errno=%d", errno);
        result = ESP_ERR_TIMEOUT;
    }
    char request[512];
    int request_size = snprintf(request, sizeof(request),
                                "GET /%s HTTP/1.1\r\nHost: %s:%d\r\nX-Device-Token: %s\r\nRange: bytes=0-\r\nConnection: close\r\n\r\n",
                                path, host, port, token);
    ESP_LOGI("stats_http", "request_path=%s", path);
    size_t sent = 0;
    while (result == ESP_OK && sent < (size_t)request_size) {
        ssize_t written = send(socket_fd, request + sent, request_size - sent, 0);
        if (written <= 0) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        sent += written;
    }
    ESP_LOGI("stats_http", "request_bytes=%d sent=%d", request_size, (int)sent);
    uint8_t *buffer = result == ESP_OK ? malloc(RESPONSE_CAPACITY) : NULL;
    if (result == ESP_OK && !buffer) {
        result = ESP_ERR_NO_MEM;
    }
    size_t received = 0;
    size_t body_offset = 0;
    size_t content_length = 0;
    while (result == ESP_OK && body_offset == 0) {
        ssize_t count = recv(socket_fd, buffer + received, 16384, 0);
        if (count <= 0) {
            ESP_LOGW("stats_http", "header recv failed count=%d errno=%d received=%u", (int)count, errno, (unsigned int)received);
            result = ESP_ERR_TIMEOUT;
            break;
        }
        received += count;
        ESP_LOGI("stats_http", "received_chunk=%d total=%u", (int)count, (unsigned int)received);
        buffer[received] = 0;
        char *end = strstr((char *)buffer, "\r\n\r\n");
        if (end) {
            body_offset = end + 4 - (char *)buffer;
            *end = 0;
            ESP_LOGI("stats_http", "response_header:\n%s", buffer);
            char *length = strcasestr((char *)buffer, "Content-Length:");
            if (!length || (!strstr((char *)buffer, " 200 ") &&
                            !strstr((char *)buffer, " 206 "))) {
                result = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            content_length = strtoul(length + strlen("Content-Length:"), NULL, 10);
            if (content_length > RESPONSE_CAPACITY - 1 - body_offset) {
                result = ESP_ERR_NO_MEM;
                break;
            }
        } else if (received > 16384) {
            result = ESP_ERR_INVALID_RESPONSE;
            break;
        }
    }
    if (result == ESP_OK) {
        memmove(buffer, buffer + body_offset, received - body_offset);
        received -= body_offset;
        while (received < content_length) {
            ssize_t count = recv(socket_fd, buffer + received, content_length - received, 0);
            if (count <= 0) {
                result = ESP_ERR_TIMEOUT;
                break;
            }
            received += count;
        }
    }
    if (result == ESP_OK) {
        buffer[content_length] = 0;
        *data = buffer;
        *size = content_length;
    } else {
        free(buffer);
    }
    close(socket_fd);
    return result;
}
