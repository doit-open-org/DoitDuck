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
