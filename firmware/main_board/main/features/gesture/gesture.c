/*
 * The gesture feature. The biggest hand in view is read as one of palm, fist,
 * ok, like (thumb up), yeah (V) or one (index up); a gesture held for half a
 * second runs its command from robots/<name>.json "gesture" once (any api
 * method, or "nod" for a nod of the head). While "ok" is held the head turns
 * to keep the hand in the middle of the picture. Commands and the head only
 * move with control on (gesture.control from the web page, off at start);
 * recognition and the skeleton overlay always run.
 *
 * The landmarks are MediaPipe's 21 points
 * (0 wrist, then four per finger from the knuckle out: thumb 1-4, index 5-8,
 * middle 9-12, ring 13-16, pinky 17-20). Every measure is a ratio of two
 * lengths on the hand, so it does not change with distance from the camera.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "api.h"
#include "camera.h"
#include "duck.h"
#include "event.h"
#include "feature.h"
#include "hand.h"

#define MAX_COMMANDS 8
#define HOLD_S 0.5          /* a gesture counts once seen this long without a break */
#define FOLLOW_GAIN 0.1f    /* head step per frame for the hand at the picture's edge */
#define FOLLOW_DEAD 0.1f    /* no head move while the hand is this near the middle */

enum { G_NONE, G_PALM, G_FIST, G_OK, G_LIKE, G_YEAH, G_ONE, G_COUNT };
static const char *const NAMES[G_COUNT] = {"", "palm", "fist", "ok", "like", "yeah", "one"};

/* MediaPipe's hand connections, for the overlay. */
static const uint8_t BONES[][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 4}, {0, 5}, {5, 6}, {6, 7}, {7, 8}, {5, 9}, {9, 10}, {10, 11},
    {11, 12}, {9, 13}, {13, 14}, {14, 15}, {15, 16}, {13, 17}, {17, 18}, {18, 19}, {19, 20}, {0, 17},
};
#define NBONES (int)(sizeof BONES / sizeof BONES[0])

struct command {
    int gesture;
    char method[32];
    cJSON *params;
};

static struct command commands[MAX_COMMANDS];
static int ncommands, control, stable, model_ok;
static char last_call[32];
static const char *last_result = "";
static os_mutex_t lock = OS_MUTEX_INIT;

static int configure(const cJSON *section)
{
    const cJSON *e;
    if (hand_open() != 0) return -1;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(section, "commands")) {
        const cJSON *g = cJSON_GetObjectItemCaseSensitive(e, "gesture");
        const cJSON *call = cJSON_GetObjectItemCaseSensitive(e, "call");
        if (ncommands == MAX_COMMANDS || !cJSON_IsString(g) || !cJSON_IsString(call)) continue;
        int k = G_COUNT;
        while (--k > G_NONE && strcmp(g->valuestring, NAMES[k])) {}
        if (k == G_NONE) {
            printf("gesture: no gesture \"%s\"\n", g->valuestring);
            continue;
        }
        struct command *c = &commands[ncommands++];
        c->gesture = k;
        snprintf(c->method, sizeof c->method, "%s", call->valuestring);
        c->params = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(e, "params"), 1);
    }
    return 0;
}

static void status(cJSON *r)
{
    os_lock(&lock);
    cJSON_AddBoolToObject(r, "ok", model_ok);
    cJSON_AddStringToObject(r, "gesture", NAMES[stable]);
    cJSON_AddBoolToObject(r, "control", control);
    cJSON_AddStringToObject(r, "last_call", last_call);
    cJSON_AddStringToObject(r, "last_result", last_result);
    os_unlock(&lock);
}

static const char *gesture_control(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)r;
    os_lock(&lock);
    control = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "on"));
    os_unlock(&lock);
    return NULL;
}

static float dist(const struct hand *h, int a, int b)
{
    return hypotf(h->x[a] - h->x[b], h->y[a] - h->y[b]);
}

/* A finger is out when it is nearly straight and its tip reaches well past the palm. */
static int finger_out(const struct hand *h, int knuckle)
{
    const float walked = dist(h, knuckle, knuckle + 1) + dist(h, knuckle + 1, knuckle + 2) + dist(h, knuckle + 2, knuckle + 3);
    return dist(h, knuckle, knuckle + 3) > 0.8f * walked && dist(h, 0, knuckle + 3) > 1.1f * dist(h, 0, 9);
}

static int classify(const struct hand *h)
{
    const float palm_width = dist(h, 5, 17), palm_length = dist(h, 0, 9);
    if (palm_width <= 0 || palm_length <= 0) return G_NONE;
    const int index = finger_out(h, 5), middle = finger_out(h, 9), ring = finger_out(h, 13), pinky = finger_out(h, 17);
    /* ok: thumb and index tips together, the other three out. */
    if (dist(h, 4, 8) < 0.45f * palm_width && middle && ring && pinky) return G_OK;
    if (index && middle && ring && pinky) return G_PALM;
    if (index && middle && !ring && !pinky) return G_YEAH;
    if (index && !middle && !ring && !pinky) return G_ONE;
    if (index || middle || ring || pinky) return G_NONE;
    /* All four curled: a thumb standing up, away from the fist, is a like. */
    const int thumb_up = dist(h, 4, 5) > 0.9f * palm_width && h->y[2] - h->y[4] > 0.5f * palm_length;
    return thumb_up ? G_LIKE : G_FIST;
}

static const char *set_head(const float head[4])
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddItemToObject(p, "head", cJSON_CreateFloatArray(head, 4));
    const char *err = NULL;
    cJSON_Delete(api_call(SRC_GESTURE, "robot.head", p, &err));
    cJSON_Delete(p);
    return err;
}

/* "call": "nod" in the config: the head dips twice and comes back where it was. */
static const char *nod(void)
{
    float head[4], down[4];
    os_lock(&d.lock);
    memcpy(head, d.head, sizeof head);
    os_unlock(&d.lock);
    memcpy(down, head, sizeof down);
    down[1] = head[1] + 0.4f;
    const char *err = NULL;
    for (int k = 0; k < 2 && !err; k++) {
        err = set_head(down);
        os_sleep(0.25);
        if (!err) err = set_head(head);
        os_sleep(0.25);
    }
    return err;
}

static void run(int g)
{
    for (int i = 0; i < ncommands; i++) {
        if (commands[i].gesture != g) continue;
        const char *err = NULL;
        if (!strcmp(commands[i].method, "nod")) err = nod();
        else cJSON_Delete(api_call(SRC_GESTURE, commands[i].method, commands[i].params, &err));
        printf("gesture: %s -> %s: %s\n", NAMES[g], commands[i].method, err ? err : "ok");
        os_lock(&lock);
        snprintf(last_call, sizeof last_call, "%s", commands[i].method);
        last_result = err ? err : "done";
        os_unlock(&lock);
        event_post(err ? EV_COMMAND_REJECTED : EV_COMMAND_OK, NAMES[g]);
    }
}

/* Turns the head a step towards the hand, so the hand comes to the middle of the picture. */
static void follow(const struct hand *h)
{
    const float ex = h->x[9] - 0.5f, ey = h->y[9] - 0.5f;
    if (fabsf(ex) < FOLLOW_DEAD && fabsf(ey) < FOLLOW_DEAD) return;
    float head[4];
    os_lock(&d.lock);
    memcpy(head, d.head, sizeof head);
    os_unlock(&d.lock);
    /* Head 2 is yaw (+ left), head 1 pitch (+ down); the hand right of the middle turns the head right. */
    if (fabsf(ex) >= FOLLOW_DEAD) head[2] -= FOLLOW_GAIN * 2 * ex;
    if (fabsf(ey) >= FOLLOW_DEAD) head[1] += FOLLOW_GAIN * 2 * ey;
    set_head(head);
}

static void task(void *arg)
{
    (void)arg;
    struct hand hands[2];
    float seg[2 * NBONES][4];
    int seen = G_NONE;
    double since = 0;
    for (;;) {
        /* -1 until the microphone has opened the camera with the audio. */
        const int n = hand_read(hands, 2);
        os_lock(&lock);
        model_ok = n >= 0;
        const int on = control;
        os_unlock(&lock);
        if (n < 0) {
            os_sleep(1);
            continue;
        }
        /* The biggest hand is the nearest one; it alone gives the gesture. */
        int big = -1;
        for (int i = 0; i < n; i++) {
            if (big < 0 || dist(&hands[i], 0, 9) > dist(&hands[big], 0, 9)) big = i;
        }
        const int g = big < 0 ? G_NONE : classify(&hands[big]);
        const double now = os_now();
        if (g != seen) {
            seen = g;
            since = now;
        } else if (g != stable && now - since >= HOLD_S) {
            os_lock(&lock);
            stable = g;
            os_unlock(&lock);
            printf("gesture: %s\n", g ? NAMES[g] : "none");
            if (on) run(g);
        }
        const int following = on && stable == G_OK && big >= 0;
        if (following) follow(&hands[big]);

        int ns = 0;
        for (int i = 0; i < n; i++) {
            for (int b = 0; b < NBONES; b++) {
                const int p = BONES[b][0], q = BONES[b][1];
                seg[ns][0] = hands[i].x[p];
                seg[ns][1] = hands[i].y[p];
                seg[ns][2] = hands[i].x[q];
                seg[ns][3] = hands[i].y[q];
                ns++;
            }
        }
        /* Red while the head follows, yellow for a recognized gesture, green otherwise. */
        camera_lines((const float (*)[4])seg, ns, following ? 0xFF4030 : stable ? 0xFFD020 : 0x40E060);
    }
}

static void start(void)
{
    os_thread(task, NULL, 0, -1);
}

static const struct api_method METHODS[] = {
    {"gesture.control", gesture_control, 1},
};

const struct feature gesture_feature = {
    .name = "gesture",
    .configure = configure,
    .start = start,
    .status = status,
    .methods = METHODS,
    .nmethods = 1,
};
