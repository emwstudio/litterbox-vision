#include "camera_ota_url.h"

#include <stdio.h>
#include <string.h>
#include "catvision_net.h"
#include "lwip/sockets.h"

bool camera_ota_url_allowed(const char *url, const char *host, int port)
{
    if (!url || !host || host[0] == '\0' || port <= 0 || port > 65535) {
        return false;
    }
    char expected[192];
    int length = snprintf(expected, sizeof(expected), "http://%s:%d/api/ota/firmware.bin", host, port);
    if (length < 0 || (size_t)length >= sizeof(expected)) {
        return false;
    }
    if (strcmp(url, expected) == 0) {
        return true;
    }
    char url_host[64];
    char path[192];
    int url_port = 0;
    if (sscanf(url, "http://%63[^:]:%d/%191[^\n]", url_host, &url_port, path) != 3 ||
        url_port != port || strcmp(path, "api/ota/firmware.bin") != 0) {
        return false;
    }
    struct sockaddr_in expected_address;
    struct sockaddr_in actual_address;
    if (catvision_resolve_ipv4(host, port, &expected_address) != ESP_OK ||
        catvision_resolve_ipv4(url_host, port, &actual_address) != ESP_OK) {
        return false;
    }
    return expected_address.sin_addr.s_addr == actual_address.sin_addr.s_addr;
}
