/*
 * The gamepad feature. robots/<name>.json "gamepad" "buttons" binds an action to
 * each button and stick ("x", "ls") and to each held button and another ("lt+x",
 * "lt+ls"): "lt+x" runs with lt held (else "x"), and a button some binding holds
 * first is held only, running nothing of its own. The sticks walk ("walk":
 * forward and back, turning; "walk_side": forward and back, sideways) and move
 * the head ("look": up and down, left and right; "neck_tilt": the neck, the
 * tilt), at once; by default the left stick walks, the right one looks.
 * An idle pad sends nothing, so it does not hold control over the web page.
 * pad.pair pairs a BLE pad, pad.unpair forgets it (from the web page).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "api.h"
#include "duck.h"
#include "feature.h"
#include "pad.h"

#define DEADZONE 0.1f

/* The buttons in PAD_* order, then the sticks. */
static const char *const KEYS[] = {"a", "b", "x", "y", "lb", "rb", "lt", "rt", "start", "select",
                                   "up", "down", "left", "right", "ls", "rs"};
#define NKEYS (int)(sizeof KEYS / sizeof KEYS[0])
#define NBUTTONS (NKEYS - 2)
#define MAX_BINDS 64

static struct {
    int held, key;   /* the button held first (-1 with none), the key */
    char name[24], action[32];
} binds[MAX_BINDS];
static int nbinds;
static uint32_t mods;                   /* the buttons some binding holds first */
static const char *active[NBUTTONS];   /* the action a held button started, ended when it is let go */
static uint32_t prev_buttons;
static int moving, heading, connected;  /* the last robot.move / robot.head sent was not all zero */
static const char *pair_state = "";    /* pad.pair: "pairing", then "" (paired) or "not_found" */
static const char *missing;            /* pad_missing() at start */

static int configure(const cJSON *section)
{
    missing = pad_missing();
    if (missing) printf("gamepad: %s: the image cannot use a BLE gamepad\n", missing);
    const cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(section, "buttons")) {
        const char *plus = strchr(v->string, '+'), *key = plus ? plus + 1 : v->string;
        const size_t n = plus ? (size_t)(plus - v->string) : 0;  /* the held button's name, before the + */
        int held = -1, k = -1;
        for (int i = 0; i < NKEYS; i++) {
            if (plus && i < NBUTTONS && strlen(KEYS[i]) == n && !strncmp(v->string, KEYS[i], n)) held = i;
            if (!strcmp(key, KEYS[i])) k = i;
        }
        if (!cJSON_IsString(v) || k < 0 || (plus && (held < 0 || held == k)) || nbinds == MAX_BINDS) {
            printf("gamepad: %s: not a binding\n", v->string);
            continue;
        }
        binds[nbinds].held = held;
        binds[nbinds].key = k;
        snprintf(binds[nbinds].name, sizeof binds[nbinds].name, "%s", v->string);
        snprintf(binds[nbinds].action, sizeof binds[nbinds].action, "%s", v->valuestring);
        if (held >= 0) mods |= 1u << held;
        nbinds++;
    }
    return 0;
}

/* An error is logged once while it repeats: held sticks send robot.move 50 times a second. */
static void call(const char *method, cJSON *params)
{
    static const char *last_err;
    const char *err = NULL;
    cJSON_Delete(api_call(SRC_PAD, method, params, &err));
    cJSON_Delete(params);
    if (err && err != last_err) printf("gamepad: %s: %s\n", method, err);
    last_err = err;
}

static void set_mode(const char *mode)
{
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "mode", mode);
    call("robot.set_mode", p);
}

static int mode_now(void)
{
    os_lock(&d.lock);
    const int m = d.mode;
    os_unlock(&d.lock);
    return m;
}

/* A button went down (pressed = 1) or up (pressed = 0). */
static void action(const char *a, int pressed)
{
    if (!strcmp(a, "start_cycle") && pressed) {
        /* Torque on and stand, then walk, then stand again. */
        const int m = mode_now();
        set_mode(m == MODE_HOLD ? "walk" : "hold");
    } else if (!strcmp(a, "estop") && pressed) {
        call("robot.estop", NULL);
    } else if (!strcmp(a, "mouth_hold")) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "deg", pressed ? C.mouth_max : 0);
        call("robot.mouth", p);
    } else if (!strncmp(a, "skill:", 6) && pressed) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", a + 6);
        call("robot.skill", p);
    }
}

static float dz(float v)
{
    return fabsf(v) < DEADZONE ? 0 : v;
}

/* Key k's action with buttons held: a binding with one of them held first, else its own, else NULL. */
static const char *bound(int k, uint32_t buttons)
{
    const char *own = NULL;
    for (int i = 0; i < nbinds; i++) {
        if (binds[i].key != k) continue;
        if (binds[i].held < 0) own = binds[i].action;
        else if (buttons & (1u << binds[i].held)) return binds[i].action;
    }
    return own;
}

static void update(const struct pad_state *s)
{
    connected = 1;
    for (int i = 0; i < NBUTTONS; i++) {
        const uint32_t bit = 1u << i;
        if ((mods & bit) || !((s->buttons ^ prev_buttons) & bit)) continue;
        if (s->buttons & bit) {
            active[i] = bound(i, s->buttons);
            if (active[i]) action(active[i], 1);
        } else if (active[i]) {
            action(active[i], 0);
            active[i] = NULL;
        }
    }
    prev_buttons = s->buttons;

    /* Forward, left and turning left positive; the head [neck, pitch, yaw, roll] as the web page's sticks send it. */
    float tw[3] = {0}, h[4] = {0};
    for (int k = 0; k < 2; k++) {
        const float x = dz(k ? s->rx : s->lx), y = dz(k ? s->ry : s->ly);
        const char *a = bound(NBUTTONS + k, s->buttons);
        if (!a) a = k ? "look" : "walk";
        if (!strcmp(a, "walk")) {
            tw[0] += y;
            tw[2] -= x;
        } else if (!strcmp(a, "walk_side")) {
            tw[0] += y;
            tw[1] -= x;
        } else if (!strcmp(a, "look")) {
            h[1] -= y;
            h[2] -= x;
        } else if (!strcmp(a, "neck_tilt")) {
            h[0] -= y;
            h[3] += x;
        }
    }
    /* Each sends one last zero when let go, then nothing, so an idle pad holds no control. */
    const int move = tw[0] != 0 || tw[1] != 0 || tw[2] != 0, head = h[0] != 0 || h[1] != 0 || h[2] != 0 || h[3] != 0;
    if (move || moving) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "vx", tw[0] * C.vmax[SRC_PAD][0]);
        cJSON_AddNumberToObject(p, "vy", tw[1] * C.vmax[SRC_PAD][1]);
        cJSON_AddNumberToObject(p, "wz", tw[2] * C.vmax[SRC_PAD][2]);
        call("robot.move", p);
    }
    if (head || heading) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddItemToObject(p, "head", cJSON_CreateFloatArray(h, 4));
        call("robot.head", p);
    }
    moving = move;
    heading = head;
}

static void disconnected(void)
{
    connected = 0;
    prev_buttons = 0;
    for (int i = 0; i < NBUTTONS; i++) {
        if (active[i]) action(active[i], 0); /* a held mouth closes */
        active[i] = NULL;
    }
    moving = heading = 0;
    call("robot.stop", NULL);
}

static void task(void *arg)
{
    (void)arg;
    struct pad_state s;
    for (;;) {
        const int r = pad_read(&s);
        if (r > 0) update(&s);
        else if (connected) disconnected();
        if (r <= 0) os_sleep(0.02);
    }
}

static void start(void)
{
    os_thread(task, NULL, 0, -1);
}

static const char *pad_status(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)p;
    cJSON_AddBoolToObject(r, "connected", connected);
    char name[96] = "";
    pad_paired(name, sizeof name);
    cJSON_AddStringToObject(r, "pair", pair_state);
    cJSON_AddStringToObject(r, "paired", name);
    cJSON_AddStringToObject(r, "missing", missing ? missing : "");
    cJSON *b = cJSON_AddObjectToObject(r, "buttons");
    for (int i = 0; i < nbinds; i++) cJSON_AddStringToObject(b, binds[i].name, binds[i].action);
    return NULL;
}

static void pair_task(void *arg)
{
    (void)arg;
    char name[64];
    const int ok = pad_pair(name, sizeof name) == 0;
    printf("gamepad: %s %s\n", ok ? "paired" : "no pad found to pair", ok ? name : "");
    pair_state = ok ? "" : "not_found";
}

/* Starts pairing, which takes up to a minute; pad.status follows it. */
static const char *pad_pair_method(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)p;
    (void)r;
    if (strcmp(pair_state, "pairing")) {
        pair_state = "pairing";
        os_thread(pair_task, NULL, 0, -1);
    }
    return NULL;
}

static const char *pad_unpair_method(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)p;
    (void)r;
    if (!strcmp(pair_state, "pairing")) return "pad_pairing";
    pair_state = "";
    return pad_unpair() == 0 ? NULL : "pad_unpair";
}

static const struct api_method METHODS[] = {
    {"pad.status", pad_status, 1},
    {"pad.pair", pad_pair_method, 1},
    {"pad.unpair", pad_unpair_method, 1},
};

const struct feature gamepad_feature = {
    .name = "gamepad",
    .configure = configure,
    .start = start,
    .methods = METHODS,
    .nmethods = 3,
};
