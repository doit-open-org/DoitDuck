#ifndef NET_H
#define NET_H

/*
 * HTTPS requests, and for xiaozhi's conversation one secure WebSocket, or one
 * MQTT connection with xiaozhi's UDP audio channel. Implementations: mbedtls.c,
 * none.c. What comes in goes to the handler given to the connect or open call,
 * from the implementation's own thread.
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

struct net_mqtt_handler {
    void (*message)(const char *text);
    void (*closed)(void);
};

/* MQTT 3.1.1 with a clean session, TLS on port 8883; subscribe "" for none (the broker sends to the client id). */
int net_mqtt_connect(const char *host, const char *port, const char *client_id, const char *username,
                     const char *password, int keepalive_s, const char *subscribe, const struct net_mqtt_handler *h);
int net_mqtt_publish(const char *topic, const char *text); /* QoS 0 */
void net_mqtt_close(void);                                 /* returns once the reader has stopped */

/* xiaozhi's UDP audio: each packet a 16-byte header (the server's nonce with the length, a timestamp and a
 * sequence number) and the payload in AES-128-CTR with that header as the counter block. A packet lost
 * on the way (up to 3 in a row) comes to receive as NULL, 0. */
int net_udp_open(const char *host, int port, const uint8_t key[16], const uint8_t nonce[16],
                 void (*receive)(const uint8_t *data, size_t len));
int net_udp_send(const uint8_t *data, size_t len);
void net_udp_close(void);

#endif
