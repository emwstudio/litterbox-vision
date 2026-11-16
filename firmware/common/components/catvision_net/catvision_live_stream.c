#include "catvision_live_stream.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"

#ifdef ESP_PLATFORM
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#else
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define LIVE_HEADER_SIZE 16

static esp_err_t read_exact(catvision_live_stream_t *stream, uint8_t *target, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = recv(stream->socket_fd, target + offset, size - offset, 0);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            ESP_LOGW("stream", "recv count=%d errno=%d remaining=%u", (int)count, errno, (unsigned int)(size - offset));
            return ESP_ERR_INVALID_RESPONSE;
        }
        offset += count;
    }
    return ESP_OK;
}

static esp_err_t write_all(catvision_live_stream_t *stream, const uint8_t *data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = send(stream->socket_fd, data + offset, size - offset, 0);
        if (count <= 0) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        offset += count;
    }
    return ESP_OK;
}

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void write_u32(uint8_t *data, uint32_t value)
{
    for (int index = 0; index < 4; index++) {
        data[index] = value >> (index * 8);
    }
}

static esp_err_t connect_socket(catvision_live_stream_t *stream, const char *host, int port)
{
    char service[6];
    snprintf(service, sizeof(service), "%d", port);
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(host, service, &hints, &addresses) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) {
            continue;
        }
        struct timeval timeout = {.tv_sec = 5};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        int enabled = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            stream->socket_fd = fd;
            break;
        }
        close(fd);
    }
    freeaddrinfo(addresses);
    return stream->socket_fd >= 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t catvision_live_stream_open(catvision_live_stream_t *stream, const char *url, int port, const char *target, const char *token, size_t max_bytes)
{
    if (!stream || !url || !target || max_bytes == 0 || port <= 0 || port > 65535) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(stream, 0, sizeof(*stream));
    stream->socket_fd = -1;
    stream->maximum = max_bytes;
    const char *scheme = strstr(url, "://");
    const char *host_start = scheme ? scheme + 3 : url;
    const char *host_end = host_start + strcspn(host_start, ":/");
    size_t host_length = host_end - host_start;
    char host[64];
    if (host_length == 0 || host_length >= sizeof(host)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(host, host_start, host_length);
    host[host_length] = 0;
    if (connect_socket(stream, host, port) != ESP_OK) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t token_length = token ? strlen(token) : 0;
    uint8_t target_index = strcmp(target, "monitor") == 0 ? 0 : strcmp(target, "stats_top") == 0 ? 1 : 2;
    if (target_index >= 2 || token_length > 128) {
        catvision_live_stream_close(stream);
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t handshake[8] = {'C', 'V', 'L', 'T', target_index, (uint8_t)token_length, 0, 0};
    if (write_all(stream, handshake, sizeof(handshake)) != ESP_OK ||
        write_all(stream, (const uint8_t *)token, token_length) != ESP_OK) {
        catvision_live_stream_close(stream);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t response[4];
    if (read_exact(stream, response, sizeof(response)) != ESP_OK || memcmp(response, "CVOK", 4) != 0) {
        catvision_live_stream_close(stream);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t read_header(catvision_live_stream_t *stream, uint32_t *content_size, uint32_t *frame_crc, bool *delta)
{
    uint8_t header[LIVE_HEADER_SIZE];
    esp_err_t result = read_exact(stream, header, sizeof(header));
    if (result != ESP_OK) {
        return result;
    }
    *content_size = read_u32(header + 4);
    if (memcmp(header, "CVP1", 4) != 0 || header[13] != 0 || header[14] != 8 || header[15] != 0 ||
        *content_size == 0 || *content_size > stream->maximum) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *frame_crc = read_u32(header + 8);
    *delta = (header[12] & 1) != 0;
    return ESP_OK;
}

esp_err_t catvision_live_stream_read_buffer(catvision_live_stream_t *stream, uint8_t *data, size_t capacity, size_t *size, uint32_t *frame_crc, bool *delta)
{
    if (!stream || stream->socket_fd < 0 || !data || !size || !frame_crc || !delta) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t content_size;
    esp_err_t result = read_header(stream, &content_size, frame_crc, delta);
    if (result != ESP_OK) {
        return result;
    }
    if (content_size > capacity) {
        return ESP_ERR_NO_MEM;
    }
    result = read_exact(stream, data, content_size);
    if (result != ESP_OK) {
        return result;
    }
    *size = content_size;
    return ESP_OK;
}

esp_err_t catvision_live_stream_read(catvision_live_stream_t *stream, uint8_t **data, size_t *size, uint32_t *frame_crc, bool *delta)
{
    if (!stream || stream->socket_fd < 0 || !data || !size || !frame_crc || !delta) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t content_size;
    esp_err_t result = read_header(stream, &content_size, frame_crc, delta);
    if (result != ESP_OK) {
        return result;
    }
    if (content_size > stream->capacity) {
        uint8_t *resized = realloc(stream->data, content_size);
        if (!resized) {
            return ESP_ERR_NO_MEM;
        }
        stream->data = resized;
        stream->capacity = content_size;
    }
    result = read_exact(stream, stream->data, content_size);
    if (result != ESP_OK) {
        return result;
    }
    *data = stream->data;
    *size = content_size;
    return ESP_OK;
}

esp_err_t catvision_live_stream_ack(catvision_live_stream_t *stream, uint32_t frame_crc)
{
    if (!stream || stream->socket_fd < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t acknowledgement[8] = {'C', 'V', 'A', 'K', 0, 0, 0, 0};
    write_u32(acknowledgement + 4, frame_crc);
    return write_all(stream, acknowledgement, sizeof(acknowledgement));
}

void catvision_live_stream_close(catvision_live_stream_t *stream)
{
    if (!stream) {
        return;
    }
    if (stream->socket_fd >= 0) {
        close(stream->socket_fd);
    }
    free(stream->data);
    memset(stream, 0, sizeof(*stream));
    stream->socket_fd = -1;
}
