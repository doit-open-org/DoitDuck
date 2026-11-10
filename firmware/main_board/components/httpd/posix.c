/*
 * httpd.h on POSIX sockets: one thread per connection. WebSocket frames per
 * RFC 6455, server side.
 */
#define _GNU_SOURCE
#include "httpd.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "osal.h"

#define WS_MAX 65536
#define BODY_MAX (4 << 20)

static const struct httpd_routes *routes;
static int srv;

static uint32_t rol(uint32_t x, int n)
{
    return x << n | x >> (32 - n);
}

/* Enough SHA-1 for the WebSocket handshake. */
static void sha1(const uint8_t *msg, size_t len, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const uint64_t bits = (uint64_t)len * 8;
    const size_t total = ((len + 8) / 64 + 1) * 64;
    for (size_t off = 0; off < total; off += 64) {
        uint8_t block[64];
        uint32_t w[80];
        for (size_t i = 0; i < 64; i++) {
            const size_t k = off + i;
            block[i] = k < len ? msg[k] : k == len ? 0x80 : 0;
        }
        if (off + 64 == total) {
            for (int i = 0; i < 8; i++) block[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
        }
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 | (uint32_t)block[4 * i + 2] << 8 |
                   block[4 * i + 3];
        }
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d5 = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            const uint32_t f = i < 20 ? (b & c) | (~b & d5) : i < 40 ? b ^ c ^ d5 : i < 60 ? (b & c) | (b & d5) | (c & d5) : b ^ c ^ d5;
            const uint32_t k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d5;
            d5 = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d5;
        h[4] += e;
    }
    for (int i = 0; i < 20; i++) out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
}

static void base64(const uint8_t *in, size_t len, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < len ? in[i + 1] << 8 : 0) | (i + 2 < len ? in[i + 2] : 0);
        out[o++] = t[v >> 18 & 63];
        out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < len ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < len ? t[v & 63] : '=';
    }
    out[o] = 0;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        const ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len > 0) {
        const ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void reply(int fd, int code, const char *type, const char *body, size_t len)
{
    char head[256];
    const int n = snprintf(head, sizeof head,
                           "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                           "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                           code, code == 200 ? "OK" : code == 400 ? "Bad Request" : "Not Found", type, len);
    send_all(fd, head, (size_t)n);
    send_all(fd, body, len);
}

static int ws_send(int fd, int opcode, const char *data, size_t len)
{
    uint8_t head[10] = {(uint8_t)(0x80 | opcode)};
    size_t n = 2;
    if (len < 126) {
        head[1] = (uint8_t)len;
    } else if (len < 65536) {
        head[1] = 126;
        head[2] = (uint8_t)(len >> 8);
        head[3] = (uint8_t)len;
        n = 4;
    } else {
        head[1] = 127;
        for (int i = 0; i < 8; i++) head[2 + i] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
        n = 10;
    }
    return send_all(fd, head, n) || send_all(fd, data, len) ? -1 : 0;
}

/* Reads one frame into a malloc'd buffer; returns the opcode, or -1 when the connection is gone. */
static int ws_read(int fd, char **data, size_t *len)
{
    uint8_t h[2], mask[4];
    if (recv_all(fd, h, 2)) return -1;
    uint64_t n = h[1] & 0x7F;
    if (n == 126) {
        uint8_t e[2];
        if (recv_all(fd, e, 2)) return -1;
        n = (uint64_t)e[0] << 8 | e[1];
    } else if (n == 127) {
        uint8_t e[8];
        if (recv_all(fd, e, 8)) return -1;
        n = 0;
        for (int i = 0; i < 8; i++) n = n << 8 | e[i];
    }
    if (n > WS_MAX || ((h[1] & 0x80) && recv_all(fd, mask, 4))) return -1;
    char *p = malloc((size_t)n + 1);
    if (!p || recv_all(fd, p, (size_t)n)) {
        free(p);
        return -1;
    }
    if (h[1] & 0x80) {
        for (uint64_t i = 0; i < n; i++) p[i] ^= (char)mask[i % 4];
    }
    p[n] = 0;
    *data = p;
    *len = (size_t)n;
    return h[0] & 0x0F;
}

int httpd_ws_send(int ws, int binary, const void *data, size_t len)
{
    return ws_send(ws, binary ? 2 : 1, data, len);
}

int httpd_ws_recv(int ws, char **data, size_t *len, double timeout)
{
    for (;;) {
        struct pollfd p = {ws, POLLIN, 0};
        const int r = poll(&p, 1, timeout < 0 ? -1 : (int)(timeout * 1000));
        if (r < 0) return -1;
        if (r == 0) return 0;
        const int op = ws_read(ws, data, len);
        if (op < 0 || op == 8) {
            if (op == 8) free(*data);
            return -1;
        }
        if (op == 1 || op == 2) return op;
        if (op == 9) ws_send(ws, 10, *data, *len); /* ping -> pong */
        free(*data);
    }
}

void httpd_ws_close(int ws)
{
    shutdown(ws, SHUT_RDWR);
}

static void ws_accept(int fd, const char *key)
{
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char joined[128], accept[32], head[256];
    uint8_t digest[20];
    snprintf(joined, sizeof joined, "%s%s", key, guid);
    sha1((const uint8_t *)joined, strlen(joined), digest);
    base64(digest, 20, accept);
    const int n = snprintf(head, sizeof head,
                           "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: %s\r\n\r\n",
                           accept);
    send_all(fd, head, (size_t)n);
}

static const char *header(const char *req, const char *name, char *buf, size_t size)
{
    const char *p = strcasestr(req, name);
    if (!p) return NULL;
    p += strlen(name);
    while (*p == ' ') p++;
    size_t n = strcspn(p, "\r\n");
    if (n >= size) n = size - 1;
    memcpy(buf, p, n);
    buf[n] = 0;
    return buf;
}

static void serve_file(int fd, const char *path)
{
    char file[192];
    snprintf(file, sizeof file, "%s%s", routes->root, strcmp(path, "/") ? path : "/index.html");
    const char *ext = strrchr(file, '.');
    const char *type = !ext ? "text/plain"
                     : !strcmp(ext, ".html") ? "text/html; charset=utf-8"
                     : !strcmp(ext, ".js") ? "text/javascript; charset=utf-8"
                     : !strcmp(ext, ".css") ? "text/css"
                     : !strcmp(ext, ".svg") ? "image/svg+xml"
                     : !strcmp(ext, ".json") ? "application/json"
                     : "application/octet-stream";
    struct stat st;
    const int f = strstr(file, "..") ? -1 : open(file, O_RDONLY);
    if (f < 0 || fstat(f, &st) != 0) {
        reply(fd, 404, "text/plain", "not found", 9);
    } else {
        char head[256];
        const int n = snprintf(head, sizeof head,
                               "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                               "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                               type, (long)st.st_size);
        send_all(fd, head, (size_t)n);
        off_t off = 0;
        while (off < st.st_size && sendfile(fd, f, &off, (size_t)(st.st_size - off)) > 0) {}
    }
    if (f >= 0) close(f);
}

static void handle(void *arg)
{
    const int fd = (int)(intptr_t)arg;
    char req[8192], method[8], path[128], value[64];
    size_t len = 0;
    long body_len = 0;
    char *head_end = NULL, *body = NULL;
    const struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (!head_end) {
        const ssize_t n = recv(fd, req + len, sizeof req - 1 - len, 0);
        if (n <= 0) goto done;
        len += (size_t)n;
        req[len] = 0;
        head_end = strstr(req, "\r\n\r\n");
        if (!head_end && len == sizeof req - 1) goto done;
    }
    /* The body, whole, wherever the headers ended: an imported policy is about a megabyte. */
    body_len = header(req, "\r\nContent-Length:", value, sizeof value) ? atol(value) : 0;
    if (body_len < 0 || body_len > BODY_MAX || !(body = malloc((size_t)body_len + 1))) goto done;
    const size_t got = (size_t)(req + len - (head_end + 4));
    const size_t have = got < (size_t)body_len ? got : (size_t)body_len;
    memcpy(body, head_end + 4, have);
    if (recv_all(fd, body + have, (size_t)body_len - have)) goto done;
    body[body_len] = 0;
    if (sscanf(req, "%7s %127s", method, path) != 2) goto done;
    path[strcspn(path, "?")] = 0;

    struct sockaddr_in me;
    socklen_t me_len = sizeof me;
    char ip[16] = "";
    if (routes->portal_ip && getsockname(fd, (struct sockaddr *)&me, &me_len) == 0 && inet_ntop(AF_INET, &me.sin_addr, ip, sizeof ip) &&
        !strcmp(ip, routes->portal_ip) && (!header(req, "\r\nHost:", value, sizeof value) || strcmp(value, routes->portal_ip))) {
        char head[192];
        const int n = snprintf(head, sizeof head, "HTTP/1.1 302 Found\r\nLocation: %s\r\nContent-Length: 0\r\n"
                               "Cache-Control: no-store\r\nConnection: close\r\n\r\n", routes->portal_url);
        send_all(fd, head, (size_t)n);
        goto done;
    }

    if (header(req, "\r\nSec-WebSocket-Key:", value, sizeof value)) {
        ws_accept(fd, value);
        /* A viewer that stops reading for 2 s is dropped rather than holding up the others. */
        const struct timeval snd = {2, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd);
        routes->ws(fd, path);
    } else if (!strncmp(path, "/api/", 5)) {
        int ok = 0;
        char *text = routes->api(path + 5, body, &ok);
        reply(fd, ok ? 200 : 400, "application/json", text, strlen(text));
        free(text);
    } else {
        serve_file(fd, path);
    }
done:
    free(body);
    close(fd);
}

static void serve(void *arg)
{
    (void)arg;
    for (;;) {
        const int fd = accept4(srv, NULL, NULL, SOCK_CLOEXEC); /* not in the programs wifi starts */
        if (fd < 0) continue;
        if (os_thread(handle, (void *)(intptr_t)fd, 0, -1) != 0) close(fd);
    }
}

int httpd_start(int port, const struct httpd_routes *r)
{
    const int one = 1;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    routes = r;
    srv = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(srv, 16) != 0) {
        perror("http port");
        return -1;
    }
    return os_thread(serve, NULL, 0, -1);
}
