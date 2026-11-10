#ifndef NET_H
#define NET_H

/*
 * HTTPS requests and one secure WebSocket, for xiaozhi. Implementations:
 * mbedtls.c, none.c. Received WebSocket messages go to the handler given to
 * net_ws_connect, from the implementation's own thread.
 */
#include <stddef.h>
#include <stdint.h>

struct net_ws_handler {
    void (*text)(const char *text);
    void (*binary)(const uint8_t *data, size_t len);
    void (*closed)(void);
};

/* NULL when the network works, otherwise why it cannot (shown on the web page). */
const char *net_unavailable(void);
/* https:// or http://; headers: NULL-terminated "Name: value" strings. Returns the HTTP status, or -1. */
int net_https_post(const char *url, const char *const *headers, const void *body, size_t len, char *resp, size_t size);
int net_ws_connect(const char *url, const char *const *headers, const struct net_ws_handler *h); /* 0 on success */
int net_ws_send_text(const char *text);
int net_ws_send_binary(const uint8_t *data, size_t len);
void net_ws_close(void);                                          /* returns once the reader has stopped */

#endif
