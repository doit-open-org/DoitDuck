/* net.h with no network support: xiaozhi reports why it cannot connect. */
#include "net.h"

const char *net_unavailable(void)
{
    return "no network support built in (net: none)";
}

int net_https_post(const char *url, const char *const *headers, const void *body, size_t len, char *resp, size_t size)
{
    (void)url;
    (void)headers;
    (void)body;
    (void)len;
    (void)resp;
    (void)size;
    return -1;
}

int net_ws_connect(const char *url, const char *const *headers, const struct net_ws_handler *h)
{
    (void)url;
    (void)headers;
    (void)h;
    return -1;
}

int net_ws_send_text(const char *text)
{
    (void)text;
    return -1;
}

int net_ws_send_binary(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return -1;
}

void net_ws_close(void)
{
}

int net_mqtt_connect(const char *host, const char *port, const char *client_id, const char *username,
                     const char *password, int keepalive_s, const char *subscribe, const struct net_mqtt_handler *h)
{
    (void)host;
    (void)port;
    (void)client_id;
    (void)username;
    (void)password;
    (void)keepalive_s;
    (void)subscribe;
    (void)h;
    return -1;
}

int net_mqtt_publish(const char *topic, const char *text)
{
    (void)topic;
    (void)text;
    return -1;
}

void net_mqtt_close(void)
{
}

int net_udp_open(const char *host, int port, const uint8_t key[16], const uint8_t nonce[16],
                 void (*receive)(const uint8_t *data, size_t len))
{
    (void)host;
    (void)port;
    (void)key;
    (void)nonce;
    (void)receive;
    return -1;
}

int net_udp_send(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return -1;
}

void net_udp_close(void)
{
}
