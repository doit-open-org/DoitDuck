#ifndef HTTPD_H
#define HTTPD_H

/*
 * A small web server: static files, POST /api/<method>, and WebSocket sessions
 * each on its own thread. Implementations: posix.c (one thread per connection).
 */
#include <stddef.h>

struct httpd_routes {
    const char *root;  /* directory of the static files, e.g. "web" */
    /* POST /api/<method> with the body (NUL-terminated, maybe empty): the JSON reply, freed with free(); *ok for 200. */
    char *(*api)(const char *method, const char *body, int *ok);
    /* A WebSocket opened at path; return to close it. */
    void (*ws)(int ws, const char *path);
    /* A request to the address portal_ip naming another site (a phone checking for a captive portal) goes to portal_url. */
    const char *portal_ip, *portal_url;
};

/* Serves on port from a thread of its own; 0 once listening. */
int httpd_start(int port, const struct httpd_routes *routes);
/* One message, text or binary; 0 on success. A receiver that stops reading for 2 s fails it. */
int httpd_ws_send(int ws, int binary, const void *data, size_t len);
/*
 * Waits up to timeout seconds (< 0: no limit) for a message: 1 text, 2 binary,
 * with *data (NUL-terminated, freed with free()) and *len; 0 on timeout; -1 when closed.
 */
int httpd_ws_recv(int ws, char **data, size_t *len, double timeout);
/* Makes the session's next httpd_ws_recv return -1, from another thread. */
void httpd_ws_close(int ws);

#endif
