#include "catvision_net.h"

#include <netdb.h>
#include <string.h>
#include "lwip/sockets.h"

esp_err_t catvision_resolve_ipv4(const char *host, int port, struct sockaddr_in *destination)
{
    if (!host || host[0] == '\0' || !destination || port <= 0 || port > 65535) {
        return ESP_ERR_INVALID_ARG;
    }
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_DGRAM,
    };
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(host, NULL, &hints, &addresses) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t result = ESP_ERR_NOT_FOUND;
    for (struct addrinfo *address = addresses; address; address = address->ai_next) {
        if (address->ai_family == AF_INET && address->ai_addrlen >= sizeof(struct sockaddr_in)) {
            memcpy(destination, address->ai_addr, sizeof(*destination));
            destination->sin_port = htons((uint16_t)port);
            result = ESP_OK;
            break;
        }
    }
    freeaddrinfo(addresses);
    return result;
}
