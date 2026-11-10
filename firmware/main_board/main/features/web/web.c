/*
 * The web feature: the page in web/, POST /api/<method> with JSON params, and
 * a WebSocket at /ws carrying JSON-RPC requests from the page plus a status
 * push every 100 ms. A WebSocket at /video carries the camera's H.264 frames,
 * one binary message each, sent by video_task to every viewer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "camera.h"
#include "config.h"
#include "duck.h"
#include "feature.h"
#include "httpd.h"

#define PUSH_S 0.1
#define MAX_VIEWERS 4
#define VIDEO_MAX (512 * 1024)

static os_mutex_t viewers_lock = OS_MUTEX_INIT;
static int viewers[MAX_VIEWERS], need_key[MAX_VIEWERS], nviewers;

/* {"result": ...} or {"error": {"code": ..., "message": ...}}, with the request's id when it has one. */
static char *run(const char *method, const cJSON *params, const cJSON *id, int *ok)
{
    const char *err = NULL;
    cJSON *result = api_call(SRC_WEB, method, params, &err);
    cJSON *out = cJSON_CreateObject();
    if (id) cJSON_AddItemToObject(out, "id", cJSON_Duplicate(id, 1));
    if (result) {
        cJSON_AddItemToObject(out, "result", result);
    } else {
        cJSON *e = cJSON_AddObjectToObject(out, "error");
        cJSON_AddStringToObject(e, "code", err);
        cJSON_AddStringToObject(e, "message", api_message(err));
    }
    *ok = result != NULL;
    char *text = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return text;
}

static char *api(const char *method, const char *body, int *ok)
{
    cJSON *params = body[0] ? cJSON_Parse(body) : NULL;
    char *text = run(method, params, NULL, ok);
    cJSON_Delete(params);
    return text;
}

/* {"id":1,"method":"robot.move","params":{...}} -> {"id":1,"result":...} or {"id":1,"error":{...}} */
static void ws_request(int ws, const char *text)
{
    cJSON *req = cJSON_Parse(text);
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(req, "method");
    if (cJSON_IsString(method)) {
        int ok;
        char *s = run(method->valuestring, cJSON_GetObjectItemCaseSensitive(req, "params"),
                      cJSON_GetObjectItemCaseSensitive(req, "id"), &ok);
        httpd_ws_send(ws, 0, s, strlen(s));
        free(s);
    }
    cJSON_Delete(req);
}

static void push_status(int ws)
{
    const char *err = NULL;
    cJSON *status = api_call(SRC_WEB, "robot.status", NULL, &err);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "type", "status");
    cJSON_AddItemToObject(out, "data", status);
    char *s = cJSON_PrintUnformatted(out);
    httpd_ws_send(ws, 0, s, strlen(s));
    free(s);
    cJSON_Delete(out);
}

static void status_session(int ws)
{
    double next_push = os_now();
    for (;;) {
        char *data = NULL;
        size_t len = 0;
        const double wait = next_push - os_now();
        const int op = httpd_ws_recv(ws, &data, &len, wait > 0 ? wait : 0);
        if (op < 0) break;
        if (op == 1) ws_request(ws, data);
        free(data);
        if (os_now() >= next_push) {
            push_status(ws);
            next_push += PUSH_S;
            if (next_push < os_now()) next_push = os_now() + PUSH_S;
        }
    }
    api_web_closed();
}

/* video_task sends the frames; this thread only notices the page go away. */
static void video_session(int ws)
{
    os_lock(&viewers_lock);
    const int ok = nviewers < MAX_VIEWERS;
    if (ok) {
        viewers[nviewers] = ws;
        need_key[nviewers++] = 1;
    }
    os_unlock(&viewers_lock);
    if (!ok) return;
    camera_key_frame();
    for (;;) {
        char *data = NULL;
        size_t len = 0;
        const int op = httpd_ws_recv(ws, &data, &len, -1);
        if (op < 0) break;
        free(data);
    }
    os_lock(&viewers_lock);
    for (int i = 0; i < nviewers; i++) {
        if (viewers[i] != ws) continue;
        viewers[i] = viewers[--nviewers];
        need_key[i] = need_key[nviewers];
        break;
    }
    os_unlock(&viewers_lock);
}

static void ws(int ws, const char *path)
{
    if (!strcmp(path, "/ws")) status_session(ws);
    else if (!strcmp(path, "/video")) video_session(ws);
}

/* Reads the camera all the time (with no viewer the frames are dropped) and sends each frame to every viewer. */
static void video_task(void *arg)
{
    (void)arg;
    uint8_t *buf = malloc(VIDEO_MAX);
    for (;;) {
        int key = 0;
        const int n = camera_h264_read(buf, VIDEO_MAX, &key);
        if (n < 0) {
            os_sleep(1); /* no camera, or the audio has not opened media_engine yet */
            continue;
        }
        os_lock(&viewers_lock);
        for (int i = 0; i < nviewers && n > 0; i++) {
            /* A new viewer can only start decoding from a key frame. */
            if (need_key[i] && !key) continue;
            need_key[i] = 0;
            if (httpd_ws_send(viewers[i], 1, buf, (size_t)n) != 0) httpd_ws_close(viewers[i]);
        }
        os_unlock(&viewers_lock);
    }
}

static const struct httpd_routes ROUTES = {"web", api, ws, "192.168.4.1", "http://192.168.4.1/#settings"}; /* the Wi-Fi hotspot's address */

static int configure(const cJSON *section)
{
    (void)section;
    return 0;
}

static void start(void)
{
    if (httpd_start(B.http_port, &ROUTES) != 0) return;
    printf("doitduck: web UI on port %d\n", B.http_port);
    os_thread(video_task, NULL, 0, -1);
}

const struct feature web_feature = {
    .name = "web",
    .configure = configure,
    .start = start,
};
