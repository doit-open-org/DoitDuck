/*
 * net.h with Mbed TLS: an HTTPS POST for the OTA and
 * activation requests (plain HTTP too, for the vision url the server gives as
 * http://), one secure WebSocket (RFC 6455, client side), one MQTT 3.1.1
 * connection (TLS on port 8883) and xiaozhi's UDP audio channel; each has a
 * reader thread that hands what comes to the caller's handler. Server
 * certificates are checked against DigiCert Global Root G2, the root of
 * api.tenclass.net; while the board clock is unset (1970 until NTP) their
 * dates are not checked.
 */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "psa/crypto.h"

#include "net.h"
#include "osal.h"

#define TIMEOUT_MS 10000
#define MAX_MESSAGE (256 * 1024)
#define CLOCK_SET 1704067200 /* 2024-01-01: an earlier clock was never set */

static const char ROOT_CA[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
"MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
"d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH\n"
"MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT\n"
"MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
"b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG\n"
"9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI\n"
"2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx\n"
"1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ\n"
"q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz\n"
"tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ\n"
"vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP\n"
"BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV\n"
"5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY\n"
"1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4\n"
"NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG\n"
"Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91\n"
"8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe\n"
"pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl\n"
"MrY=\n"
"-----END CERTIFICATE-----\n";

struct tls {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    int plain; /* http:// or MQTT off 8883: the socket alone, no TLS */
};

/* A connection with a reader thread: the WebSocket, MQTT. */
struct conn {
    struct tls t;
    os_mutex_t io; /* one TLS context: reads and writes take turns */
    volatile int open, stop;
    void (*idle)(void); /* on the reader's thread while nothing comes, every 0.1 s */
};

static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_x509_crt ca;
static mbedtls_ssl_config conf;
static int tls_ready;
static os_mutex_t init_lock = OS_MUTEX_INIT, rng_lock = OS_MUTEX_INIT;

static void mqtt_idle(void);
static struct conn ws = {.io = OS_MUTEX_INIT}, mq = {.io = OS_MUTEX_INIT, .idle = mqtt_idle};
static struct net_ws_handler handler;
static struct net_mqtt_handler mq_handler;
static double mq_keepalive, mq_sent;

static int verify(void *ctx, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)ctx;
    (void)crt;
    (void)depth;
    if (time(NULL) < CLOCK_SET) *flags &= ~(uint32_t)(MBEDTLS_X509_BADCERT_FUTURE | MBEDTLS_X509_BADCERT_EXPIRED);
    return 0;
}

/* The OTA requests and the WebSocket run on different threads; the generator is not thread safe. */
static int rng(void *p, unsigned char *out, size_t len)
{
    os_lock(&rng_lock);
    const int r = mbedtls_ctr_drbg_random(p, out, len);
    os_unlock(&rng_lock);
    return r;
}

static int tls_init(void)
{
    os_lock(&init_lock);
    if (!tls_ready) {
        psa_crypto_init();
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_x509_crt_init(&ca);
        mbedtls_ssl_config_init(&conf);
        int r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)"doitduck", 8);
        if (!r) r = mbedtls_x509_crt_parse(&ca, (const unsigned char *)ROOT_CA, sizeof ROOT_CA);
        if (!r) r = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                                MBEDTLS_SSL_PRESET_DEFAULT);
        if (r) {
            printf("net: tls setup failed: -0x%04x\n", (unsigned)-r);
        } else {
            mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            mbedtls_ssl_conf_ca_chain(&conf, &ca, NULL);
            mbedtls_ssl_conf_rng(&conf, rng, &drbg);
            mbedtls_ssl_conf_verify(&conf, verify, NULL);
            mbedtls_ssl_conf_read_timeout(&conf, TIMEOUT_MS);
            mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
            tls_ready = 1;
        }
    }
    os_unlock(&init_lock);
    return tls_ready ? 0 : -1;
}

static void tls_free(struct tls *t)
{
    mbedtls_ssl_free(&t->ssl);
    mbedtls_net_free(&t->net);
}

static int tls_connect(struct tls *t, const char *host, const char *port)
{
    mbedtls_net_init(&t->net);
    mbedtls_ssl_init(&t->ssl);
    if (tls_init() != 0) return -1;
    int r = mbedtls_net_connect(&t->net, host, port, MBEDTLS_NET_PROTO_TCP);
    if (t->plain && !r) return 0;
    if (!r) r = mbedtls_ssl_setup(&t->ssl, &conf);
    if (!r) r = mbedtls_ssl_set_hostname(&t->ssl, host);
    if (!r) {
        mbedtls_ssl_set_bio(&t->ssl, &t->net, mbedtls_net_send, NULL, mbedtls_net_recv_timeout);
        do r = mbedtls_ssl_handshake(&t->ssl);
        while (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE);
    }
    if (r) {
        char why[128];
        mbedtls_strerror(r, why, sizeof why);
        printf("net: tls %s:%s: %s (-0x%04x, verify 0x%x)\n", host, port, why, (unsigned)-r,
               (unsigned)mbedtls_ssl_get_verify_result(&t->ssl));
        tls_free(t);
        return -1;
    }
    return 0;
}

static int tls_write(struct tls *t, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    while (len > 0) {
        const int r = t->plain ? mbedtls_net_send(&t->net, p, len) : mbedtls_ssl_write(&t->ssl, p, len);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) return -1;
        p += r;
        len -= (size_t)r;
    }
    return 0;
}

/* "https://host[:port]/path" or "wss://..."; 0 on success. */
static int parse_url(const char *url, char *host, size_t host_size, char *port, size_t port_size, char *path,
                     size_t path_size)
{
    const int plain = !strncmp(url, "http://", 7);
    if (strncmp(url, "https://", 8) && strncmp(url, "wss://", 6) && !plain) {
        printf("net: only http://, https:// and wss:// are supported: %s\n", url);
        return -1;
    }
    const char *p = strstr(url, "://") + 3;
    const char *slash = strchr(p, '/');
    const size_t n = slash ? (size_t)(slash - p) : strlen(p);
    if (n == 0 || n >= host_size) return -1;
    memcpy(host, p, n);
    host[n] = 0;
    char *colon = strchr(host, ':');
    snprintf(port, port_size, "%s", colon ? colon + 1 : plain ? "80" : "443");
    if (colon) *colon = 0;
    snprintf(path, path_size, "%s", slash ? slash : "/");
    return 0;
}

/* The request line and headers, each header a "Name: value" string. Returns the length, or -1. */
static int request(char *buf, size_t size, const char *first, const char *const *headers)
{
    int n = snprintf(buf, size, "%s", first);
    for (int i = 0; headers[i] && n < (int)size; i++) n += snprintf(buf + n, size - (size_t)n, "%s\r\n", headers[i]);
    if (n < (int)size) n += snprintf(buf + n, size - (size_t)n, "\r\n");
    return n < (int)size ? n : -1;
}

const char *net_unavailable(void)
{
    return NULL;
}

int net_https_post(const char *url, const char *const *headers, const void *body, size_t len, char *resp, size_t size)
{
    char host[128], port[8], path[256], first[768], head[1536];
    if (parse_url(url, host, sizeof host, port, sizeof port, path, sizeof path) != 0) return -1;
    /* HTTP/1.0, so the reply is not chunked and ends when the server closes. */
    snprintf(first, sizeof first, "POST %s HTTP/1.0\r\nHost: %s\r\nContent-Length: %zu\r\n", path, host, len);
    const int n = request(head, sizeof head, first, headers);
    struct tls t = {.plain = !strncmp(url, "http://", 7)};
    if (n < 0 || tls_connect(&t, host, port) != 0) return -1;
    int status = -1;
    const size_t cap = size + 2048;
    char *buf = malloc(cap);
    if (buf && tls_write(&t, head, (size_t)n) == 0 && tls_write(&t, body, len) == 0) {
        size_t got = 0;
        for (;;) {
            const int r = t.plain ? mbedtls_net_recv_timeout(&t.net, (unsigned char *)buf + got, cap - 1 - got, 10000)
                                  : mbedtls_ssl_read(&t.ssl, (unsigned char *)buf + got, cap - 1 - got);
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (r <= 0) break;
            got += (size_t)r;
            if (got == cap - 1) break;
        }
        buf[got] = 0;
        const char *b = strstr(buf, "\r\n\r\n");
        if (b && sscanf(buf, "HTTP/%*s %d", &status) == 1) snprintf(resp, size, "%s", b + 4);
    }
    free(buf);
    tls_free(&t);
    return status;
}

/* Reads exactly n bytes; -1 when the connection ends or was asked to close. */
static int conn_read(struct conn *c, uint8_t *p, size_t n)
{
    while (n > 0) {
        if (c->stop) return -1;
        /* Wait outside the lock, so the senders are not held up by an idle line. */
        if (c->t.plain || !mbedtls_ssl_get_bytes_avail(&c->t.ssl)) {
            struct pollfd pf = {.fd = c->t.net.fd, .events = POLLIN};
            const int r = poll(&pf, 1, 100);
            if (r < 0) return -1;
            if (r == 0) {
                if (c->idle) c->idle();
                continue;
            }
        }
        os_lock(&c->io);
        const int r = c->t.plain ? mbedtls_net_recv(&c->t.net, p, n) : mbedtls_ssl_read(&c->t.ssl, p, n);
        os_unlock(&c->io);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int conn_write(struct conn *c, const void *buf, size_t len)
{
    os_lock(&c->io);
    const int r = c->open && !c->stop ? tls_write(&c->t, buf, len) : -1;
    os_unlock(&c->io);
    return r;
}

/* At the end of its reader. */
static void conn_close(struct conn *c)
{
    os_lock(&c->io);
    if (!c->t.plain) mbedtls_ssl_close_notify(&c->t.ssl);
    tls_free(&c->t);
    c->open = 0;
    os_unlock(&c->io);
}

static int ws_send(int opcode, const void *data, size_t len)
{
    uint8_t *f = malloc(len + 14);
    if (!f) return -1;
    size_t h = 0;
    f[h++] = (uint8_t)(0x80 | opcode);
    if (len < 126) {
        f[h++] = (uint8_t)(0x80 | len);
    } else if (len < 65536) {
        f[h++] = 0x80 | 126;
        f[h++] = (uint8_t)(len >> 8);
        f[h++] = (uint8_t)len;
    } else {
        f[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) f[h++] = (uint8_t)((uint64_t)len >> (8 * i));
    }
    /* A client masks every frame (RFC 6455 5.3). */
    uint8_t *mask = f + h;
    for (int i = 0; i < 4; i++) mask[i] = (uint8_t)rand();
    h += 4;
    for (size_t i = 0; i < len; i++) f[h + i] = ((const uint8_t *)data)[i] ^ mask[i & 3];
    const int r = conn_write(&ws, f, h + len);
    free(f);
    return r;
}

static void reader(void *arg)
{
    (void)arg;
    uint8_t *msg = NULL;
    size_t len = 0;
    int op = 0;
    for (;;) {
        uint8_t h[2], ext[8];
        if (conn_read(&ws, h, 2)) break;
        uint64_t n = h[1] & 0x7F;
        if (n == 126) {
            if (conn_read(&ws, ext, 2)) break;
            n = (uint64_t)ext[0] << 8 | ext[1];
        } else if (n == 127) {
            if (conn_read(&ws, ext, 8)) break;
            n = 0;
            for (int i = 0; i < 8; i++) n = n << 8 | ext[i];
        }
        if (len + n > MAX_MESSAGE) {
            printf("net: websocket message over %d bytes\n", MAX_MESSAGE);
            break;
        }
        uint8_t *m = realloc(msg, len + n + 1);
        if (!m) break;
        msg = m;
        if (conn_read(&ws, msg + len, n)) break;
        const int opcode = h[0] & 0x0F;
        if (opcode == 8) break;
        if (opcode == 9) ws_send(10, msg + len, n); /* ping -> pong */
        if (opcode >= 8) continue;                  /* control frames sit between fragments */
        if (opcode) op = opcode;                    /* 0 continues a fragmented message */
        len += n;
        if (!(h[0] & 0x80)) continue;
        msg[len] = 0;
        if (op == 1) handler.text((const char *)msg);
        if (op == 2) handler.binary(msg, len);
        len = 0;
    }
    free(msg);
    handler.closed();
    conn_close(&ws);
}

int net_ws_connect(const char *url, const char *const *headers, const struct net_ws_handler *h)
{
    char host[128], port[8], path[256], first[768], head[1536], resp[1024];
    if (parse_url(url, host, sizeof host, port, sizeof port, path, sizeof path) != 0) return -1;
    /* The reader of the previous connection may still be closing it. */
    for (int i = 0; ws.open && i < 100; i++) os_sleep(0.01);
    if (ws.open) return -1;
    uint8_t key[16];
    unsigned char key64[32];
    size_t key_len = 0;
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)rand();
    mbedtls_base64_encode(key64, sizeof key64, &key_len, key, sizeof key);
    snprintf(first, sizeof first,
             "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n",
             path, host, key64);
    const int n = request(head, sizeof head, first, headers);
    if (n < 0 || tls_connect(&ws.t, host, port) != 0) return -1;
    /* The reply headers a byte at a time, so nothing after them is taken. */
    size_t got = 0;
    int ok = tls_write(&ws.t, head, (size_t)n) == 0;
    while (ok && got < sizeof resp - 1 && (got < 4 || memcmp(resp + got - 4, "\r\n\r\n", 4))) {
        const int r = mbedtls_ssl_read(&ws.t.ssl, (unsigned char *)resp + got, 1);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) ok = 0;
        else got++;
    }
    resp[got] = 0;
    if (!ok || strncmp(resp, "HTTP/1.1 101", 12)) {
        printf("net: websocket refused: %.120s\n", resp);
        tls_free(&ws.t);
        return -1;
    }
    handler = *h;
    ws.stop = 0;
    ws.open = 1;
    if (os_thread(reader, NULL, 0, -1) != 0) {
        ws.open = 0;
        tls_free(&ws.t);
        return -1;
    }
    return 0;
}

int net_ws_send_text(const char *text)
{
    return ws_send(1, text, strlen(text));
}

int net_ws_send_binary(const uint8_t *data, size_t len)
{
    return ws_send(2, data, len);
}

void net_ws_close(void)
{
    ws.stop = 1;
    for (int i = 0; ws.open && i < 200; i++) os_sleep(0.01);
}

/* MQTT's remaining length: 7 bits a byte, low first. */
static size_t put_len(uint8_t *p, size_t n)
{
    size_t i = 0;
    do {
        p[i] = (uint8_t)(n & 0x7F);
        n >>= 7;
        if (n) p[i] |= 0x80;
        i++;
    } while (n);
    return i;
}

/* An MQTT string: its length in 2 bytes, then the bytes. */
static size_t put_str(uint8_t *p, const char *s)
{
    const size_t n = strlen(s);
    p[0] = (uint8_t)(n >> 8);
    p[1] = (uint8_t)n;
    memcpy(p + 2, s, n);
    return n + 2;
}

/* An MQTT packet: its type byte, the length, the body. */
static int mqtt_send(uint8_t type, const uint8_t *body, size_t len)
{
    uint8_t *f = malloc(len + 5);
    if (!f) return -1;
    f[0] = type;
    const size_t h = 1 + put_len(f + 1, len);
    memcpy(f + h, body, len);
    const int r = conn_write(&mq, f, h + len);
    free(f);
    mq_sent = os_now();
    return r;
}

/* A PINGREQ when nothing was sent for half the keep-alive, so the broker keeps the connection. */
static void mqtt_idle(void)
{
    if (os_now() - mq_sent > mq_keepalive / 2) mqtt_send(0xC0, (const uint8_t *)"", 0);
}

static void mqtt_reader(void *arg)
{
    (void)arg;
    uint8_t *buf = NULL;
    for (;;) {
        uint8_t h, b;
        size_t n = 0;
        int ok = !conn_read(&mq, &h, 1);
        for (int i = 0; ok; i++) {
            ok = i < 4 && !conn_read(&mq, &b, 1);
            if (ok) n |= (size_t)(b & 0x7F) << (7 * i);
            if (!ok || !(b & 0x80)) break;
        }
        if (!ok || n > MAX_MESSAGE) break;
        uint8_t *m = realloc(buf, n + 1);
        if (!m) break;
        buf = m;
        if (conn_read(&mq, buf, n)) break;
        buf[n] = 0;
        const int type = h >> 4, qos = (h >> 1) & 3;
        if (type == 3 && n >= 2) { /* PUBLISH: the topic, the packet id with QoS 1 or 2, the message */
            size_t at = 2 + ((size_t)buf[0] << 8 | buf[1]);
            if (qos && at + 2 <= n) {
                mqtt_send(qos == 1 ? 0x40 : 0x50, buf + at, 2); /* PUBACK, or PUBREC */
                at += 2;
            }
            if (at <= n) mq_handler.message((const char *)buf + at);
        }
        if (type == 6 && n >= 2) mqtt_send(0x70, buf, 2); /* PUBREL -> PUBCOMP */
    }
    free(buf);
    mq_handler.closed();
    conn_close(&mq);
}

int net_mqtt_connect(const char *host, const char *port, const char *client_id, const char *username,
                     const char *password, int keepalive_s, const char *subscribe, const struct net_mqtt_handler *h)
{
    /* The reader of the previous connection may still be closing it. */
    for (int i = 0; mq.open && i < 100; i++) os_sleep(0.01);
    if (mq.open) return -1;
    mq.t.plain = strcmp(port, "8883") != 0;
    if (tls_connect(&mq.t, host, port) != 0) return -1;
    mq.stop = 0;
    mq.open = 1;
    mq_keepalive = keepalive_s;
    /* CONNECT: MQTT 3.1.1, a clean session, with the user name and password. */
    uint8_t body[1024], ack[4];
    size_t n = put_str(body, "MQTT");
    body[n++] = 4;
    body[n++] = 0x80 | 0x40 | 0x02;
    body[n++] = (uint8_t)(keepalive_s >> 8);
    body[n++] = (uint8_t)keepalive_s;
    n += put_str(body + n, client_id);
    n += put_str(body + n, username);
    n += put_str(body + n, password);
    int ok = mqtt_send(0x10, body, n) == 0;
    for (size_t got = 0; ok && got < sizeof ack;) {
        const int r = mq.t.plain ? mbedtls_net_recv_timeout(&mq.t.net, ack + got, sizeof ack - got, TIMEOUT_MS)
                                 : mbedtls_ssl_read(&mq.t.ssl, ack + got, sizeof ack - got);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) ok = 0;
        else got += (size_t)r;
    }
    if (!ok || ack[0] != 0x20 || ack[3] != 0) {
        printf("net: mqtt %s:%s refused (%d)\n", host, port, ok ? ack[3] : -1);
        conn_close(&mq);
        return -1;
    }
    if (subscribe[0]) {
        n = 0;
        body[n++] = 0;
        body[n++] = 1; /* packet id */
        n += put_str(body + n, subscribe);
        body[n++] = 0; /* QoS 0 */
        mqtt_send(0x82, body, n);
    }
    mq_handler = *h;
    if (os_thread(mqtt_reader, NULL, 0, -1) != 0) {
        conn_close(&mq);
        return -1;
    }
    return 0;
}

int net_mqtt_publish(const char *topic, const char *text)
{
    const size_t nt = strlen(topic) + 2, len = strlen(text);
    uint8_t *body = malloc(nt + len);
    if (!body) return -1;
    put_str(body, topic);
    memcpy(body + nt, text, len);
    const int r = mqtt_send(0x30, body, nt + len); /* PUBLISH, QoS 0 */
    free(body);
    return r;
}

void net_mqtt_close(void)
{
    mqtt_send(0xE0, (const uint8_t *)"", 0); /* DISCONNECT */
    mq.stop = 1;
    for (int i = 0; mq.open && i < 200; i++) os_sleep(0.01);
}

static mbedtls_net_context udp;
static mbedtls_aes_context aes;
static uint8_t udp_nonce[16];
static uint32_t udp_sent, udp_got; /* the last sequence number each way */
static double udp_since;
static void (*udp_receive)(const uint8_t *data, size_t len);
static volatile int udp_open, udp_stop;

/* AES-128-CTR, the packet's header as the counter block: encrypts and decrypts. */
static void udp_crypt(const uint8_t *header, const uint8_t *in, size_t len, uint8_t *out)
{
    uint8_t counter[16], block[16];
    size_t off = 0;
    memcpy(counter, header, 16);
    mbedtls_aes_crypt_ctr(&aes, len, &off, counter, block, in, out);
}

static void udp_reader(void *arg)
{
    (void)arg;
    uint8_t p[2048], data[2048];
    while (!udp_stop) {
        struct pollfd pf = {.fd = udp.fd, .events = POLLIN};
        if (poll(&pf, 1, 100) <= 0) continue;
        const int n = mbedtls_net_recv(&udp, p, sizeof p);
        /* Type 1 (audio), and the payload length the header gives. */
        if (n < 16 || p[0] != 0x01 || (size_t)n != 16 + ((size_t)p[2] << 8 | p[3])) continue;
        const uint32_t seq = (uint32_t)p[12] << 24 | (uint32_t)p[13] << 16 | (uint32_t)p[14] << 8 | p[15];
        if (seq <= udp_got) continue; /* late or repeated */
        /* Up to 3 lost on the way (UDP is not resent): told as NULL, one each, for the decoder to fill in. */
        for (uint32_t i = udp_got + 1; udp_got && i < seq && seq - i <= 3; i++) udp_receive(NULL, 0);
        udp_got = seq;
        udp_crypt(p, p + 16, (size_t)n - 16, data);
        udp_receive(data, (size_t)n - 16);
    }
    mbedtls_net_free(&udp);
    udp_open = 0;
}

int net_udp_open(const char *host, int port, const uint8_t key[16], const uint8_t nonce[16],
                 void (*receive)(const uint8_t *data, size_t len))
{
    for (int i = 0; udp_open && i < 100; i++) os_sleep(0.01);
    if (udp_open) return -1;
    char p[8];
    snprintf(p, sizeof p, "%d", port);
    mbedtls_net_init(&udp);
    if (mbedtls_net_connect(&udp, host, p, MBEDTLS_NET_PROTO_UDP) != 0) {
        printf("net: udp %s:%s failed\n", host, p);
        return -1;
    }
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);
    memcpy(udp_nonce, nonce, 16);
    udp_sent = udp_got = 0;
    udp_since = os_now();
    udp_receive = receive;
    udp_stop = 0;
    udp_open = 1;
    if (os_thread(udp_reader, NULL, 0, -1) != 0) {
        udp_open = 0;
        mbedtls_net_free(&udp);
        return -1;
    }
    return 0;
}

int net_udp_send(const uint8_t *data, size_t len)
{
    uint8_t p[16 + 1500];
    if (!udp_open || udp_stop || len > 1500) return -1;
    /* The header: the server's nonce, with the payload length, a timestamp (ms) and the sequence number. */
    memcpy(p, udp_nonce, 16);
    p[2] = (uint8_t)(len >> 8);
    p[3] = (uint8_t)len;
    const uint32_t ts = (uint32_t)((os_now() - udp_since) * 1000), seq = ++udp_sent;
    for (int i = 0; i < 4; i++) {
        p[8 + i] = (uint8_t)(ts >> (24 - 8 * i));
        p[12 + i] = (uint8_t)(seq >> (24 - 8 * i));
    }
    udp_crypt(p, data, len, p + 16);
    return mbedtls_net_send(&udp, p, 16 + len) == (int)(16 + len) ? 0 : -1;
}

void net_udp_close(void)
{
    udp_stop = 1;
    for (int i = 0; udp_open && i < 200; i++) os_sleep(0.01);
}
