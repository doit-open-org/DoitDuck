/*
 * The gamepad feature. Sticks drive the body in move mode and the head in head
 * mode; buttons run the actions bound to them in robots/<name>.json "gamepad".
 * An idle pad sends nothing, so it does not hold control over the web page.
 * Runs only where pad.h has a reader.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "api.h"
#include "duck.h"
#include "feature.h"
#include "pad.h"

#define DEADZONE 0.1f
#define ESTOP_HOLD_S 2.0

static const char *const BUTTONS[] = {"a", "b", "x", "y", "lb", "rb", "lt", "rt", "start", "select",
                                      "up", "down", "left", "right"};
#define NBUTTONS (int)(sizeof BUTTONS / sizeof BUTTONS[0])

static char binding[NBUTTONS][32];     /* action name per button */
static uint32_t prev_buttons;
static int head_mode, sticks_active, connected;
static double estop_since;

static int configure(const cJSON *section)
{
    struct pad_state s;
    if (pad_read(&s) < 0) return -1;
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(section, "buttons");
    for (int i = 0; i < NBUTTONS; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(b, BUTTONS[i]);
        snprintf(binding[i], sizeof binding[i], "%s", cJSON_IsString(v) ? v->valuestring : "");
    }
    return 0;
}

static void call(const char *method, cJSON *params)
{
    const char *err = NULL;
    cJSON_Delete(api_call(SRC_PAD, method, params, &err));
    cJSON_Delete(params);
    if (err) printf("gamepad: %s: %s\n", method, err);
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
    } else if (!strcmp(a, "walk_toggle") && pressed) {
        set_mode(mode_now() == MODE_WALK ? "hold" : "walk");
    } else if (!strcmp(a, "head_mode") && pressed) {
        head_mode = 1;
    } else if (!strcmp(a, "move_mode") && pressed) {
        head_mode = 0;
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

static void update(const struct pad_state *s)
{
    const double now = os_now();
    connected = 1;
    for (int i = 0; i < NBUTTONS; i++) {
        const uint32_t bit = 1u << i;
        if ((s->buttons ^ prev_buttons) & bit && binding[i][0]) action(binding[i], !!(s->buttons & bit));
    }
    /* Holding the button bound to estop_hold for 2 s turns torque off. */
    int estop_held = 0;
    for (int i = 0; i < NBUTTONS; i++) {
        if (!strcmp(binding[i], "estop_hold") && (s->buttons & (1u << i))) estop_held = 1;
    }
    if (!estop_held) {
        estop_since = 0;
    } else if (estop_since == 0) {
        estop_since = now;
    } else if (now - estop_since >= ESTOP_HOLD_S) {
        call("robot.estop", NULL);
        estop_since = now + 1e9; /* once per hold */
    }
    prev_buttons = s->buttons;

    const float lx = dz(s->lx), ly = dz(s->ly), rx = dz(s->rx), ry = dz(s->ry);
    const int active = lx != 0 || ly != 0 || rx != 0 || ry != 0;
    /* Centred sticks send one last zero, then nothing, so an idle pad holds no control. */
    if (!active && !sticks_active) return;
    sticks_active = active;
    cJSON *p = cJSON_CreateObject();
    if (head_mode) {
        const float h[4] = {ly, -lx, ry, -rx};
        cJSON_AddItemToObject(p, "head", cJSON_CreateFloatArray(h, 4));
        call("robot.head", p);
    } else {
        cJSON_AddNumberToObject(p, "vx", ly * C.vmax[SRC_PAD][0]);
        cJSON_AddNumberToObject(p, "vy", -lx * C.vmax[SRC_PAD][1]);
        cJSON_AddNumberToObject(p, "wz", -rx * C.vmax[SRC_PAD][2]);
        call("robot.move", p);
    }
}

static void disconnected(void)
{
    connected = 0;
    prev_buttons = 0;
    sticks_active = 0;
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
    cJSON_AddStringToObject(r, "mode", head_mode ? "head" : "move");
    cJSON *b = cJSON_AddObjectToObject(r, "buttons");
    for (int i = 0; i < NBUTTONS; i++) {
        if (binding[i][0]) cJSON_AddStringToObject(b, BUTTONS[i], binding[i]);
    }
    return NULL;
}

static const struct api_method METHODS[] = {
    {"pad.status", pad_status, 1},
};

const struct feature gamepad_feature = {
    .name = "gamepad",
    .configure = configure,
    .start = start,
    .methods = METHODS,
    .nmethods = 1,
};
