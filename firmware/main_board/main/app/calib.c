/*
 * Calibration: the back end of the web page's
 * wizard. Four steps, each redone on its own:
 *
 *   1 servos     the web page scans the bus (servo.op) and marks the step
 *   2 imu_bias   calib.imu 1 with torque off and the duck still
 *   3 zero       calib.capture: torque off, the duck in the calibration pose
 *                (robots/<name>.json calibration.pose_deg), all zeros at once
 *   4 stand      calib.suggest measures the standing duck and proposes nudges;
 *                calib.nudge applies them; calib.imu 2 levels the torso
 *
 * Everything lives in data/calibration.json; the one before each change is
 * kept as calibration.prev.json (calib.undo). Files are written outside d.lock,
 * so the bus loop never waits on the flash. CLDuck's zero_pos.json and
 * imu_level.json are read when there is no calibration.json yet.
 */
#include "calib.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "duck.h"
#include "storage.h"

#define FILE_NAME "calibration.json"
#define PREV_NAME "calibration.prev.json"
#define MAX_NUDGE_DEG 5

enum { STEP_SERVOS, STEP_IMU_BIAS, STEP_ZERO, STEP_STAND, STEP_LEVEL, NSTEPS };
static const char *const STEPS[NSTEPS] = {"servos", "imu_bias", "zero", "stand", "level"};

static int steps[NSTEPS];
static double saved_at;           /* time() of the last save, 0 for never */
/* One change at a time; d.lock is taken only to copy in and out. */
static os_mutex_t lock = OS_MUTEX_INIT;

static double num(const cJSON *o, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

static const cJSON *cfg(const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(config_section("calibration"), key);
}

static int raw_from_deg(double deg)
{
    return (int)lround(deg * 4095.0 / 360.0 * C.motor_sign);
}

static uint16_t clamp_raw(int v)
{
    return (uint16_t)(v < 0 ? 0 : v > 4095 ? 4095 : v);
}

/* The calibration as saved: with lock held. */
static cJSON *snapshot(void)
{
    int zero[MAX_MOTORS], ids[MAX_MOTORS];
    float level[2];
    os_lock(&d.lock);
    for (int i = 0; i < C.n; i++) zero[i] = d.zero_pos[i];
    memcpy(level, d.imu_level, sizeof level);
    os_unlock(&d.lock);
    for (int i = 0; i < C.n; i++) ids[i] = C.id[i];
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "version", 1);
    cJSON_AddStringToObject(r, "robot", C.name);
    cJSON_AddNumberToObject(r, "time", saved_at);
    cJSON_AddItemToObject(r, "ids", cJSON_CreateIntArray(ids, C.n));
    cJSON_AddItemToObject(r, "zero_pos", cJSON_CreateIntArray(zero, C.n));
    cJSON_AddItemToObject(r, "imu_level", cJSON_CreateFloatArray(level, 2));
    cJSON *s = cJSON_AddObjectToObject(r, "steps");
    for (int k = 0; k < NSTEPS; k++) cJSON_AddBoolToObject(s, STEPS[k], steps[k]);
    return r;
}

/* Puts a saved calibration into d and steps, with lock held; NULL or an error code. */
static const char *apply(const cJSON *c)
{
    const cJSON *ids = cJSON_GetObjectItemCaseSensitive(c, "ids");
    const cJSON *zero = cJSON_GetObjectItemCaseSensitive(c, "zero_pos");
    const cJSON *level = cJSON_GetObjectItemCaseSensitive(c, "imu_level");
    if (cJSON_GetArraySize(ids) != C.n || cJSON_GetArraySize(zero) != C.n) return "calib_mismatch";
    for (int i = 0; i < C.n; i++) {
        const cJSON *id = cJSON_GetArrayItem(ids, i), *z = cJSON_GetArrayItem(zero, i);
        if (!cJSON_IsNumber(id) || id->valueint != C.id[i]) return "calib_mismatch";
        if (!cJSON_IsNumber(z) || z->valuedouble < 0 || z->valuedouble > 4095) return "calib_mismatch";
    }
    const int has_level = cJSON_GetArraySize(level) == 2;
    os_lock(&d.lock);
    for (int i = 0; i < C.n; i++) d.zero_pos[i] = (uint16_t)cJSON_GetArrayItem(zero, i)->valueint;
    if (has_level) {
        for (int k = 0; k < 2; k++) d.imu_level[k] = (float)cJSON_GetArrayItem(level, k)->valuedouble;
    }
    os_unlock(&d.lock);
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(c, "steps");
    for (int k = 0; k < NSTEPS; k++) steps[k] = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s, STEPS[k]));
    saved_at = num(c, "time", 0);
    return NULL;
}

/* Writes the calibration now in d, keeping the file before as the previous one; with lock held. */
static const char *save(void)
{
    char *old = store_get(FILE_NAME, NULL);
    if (old && old[0]) store_put(PREV_NAME, old, strlen(old)); /* an empty one is no calibration to go back to */
    free(old);
    saved_at = (double)time(NULL);
    cJSON *c = snapshot();
    char *text = cJSON_Print(c);
    const int rc = store_put(FILE_NAME, text, strlen(text));
    free(text);
    cJSON_Delete(c);
    return rc == 0 ? NULL : "calib_storage";
}

/* CLDuck's files, beside the program or in data/. */
static void load_old(void)
{
    char buf[2048];
    if (store_kv_get("zero_pos.json", buf, sizeof buf) > 0) {
        cJSON *root = cJSON_Parse(buf);
        const cJSON *z = cJSON_GetObjectItemCaseSensitive(root, "zero_pos");
        if (cJSON_GetArraySize(z) == C.n) {
            os_lock(&d.lock);
            for (int i = 0; i < C.n; i++) {
                const cJSON *v = cJSON_GetArrayItem(z, i);
                if (cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 4095) d.zero_pos[i] = (uint16_t)v->valueint;
            }
            os_unlock(&d.lock);
            steps[STEP_ZERO] = 1;
            printf("calibration: zero positions from zero_pos.json\n");
        }
        cJSON_Delete(root);
    }
    if (store_kv_get("imu_level.json", buf, sizeof buf) > 0) {
        cJSON *root = cJSON_Parse(buf);
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "roll"), *p = cJSON_GetObjectItemCaseSensitive(root, "pitch");
        if (cJSON_IsNumber(r) && cJSON_IsNumber(p)) {
            os_lock(&d.lock);
            d.imu_level[0] = (float)r->valuedouble;
            d.imu_level[1] = (float)p->valuedouble;
            os_unlock(&d.lock);
            steps[STEP_LEVEL] = 1;
            printf("calibration: imu level from imu_level.json\n");
        }
        cJSON_Delete(root);
    }
}

void calib_load(void)
{
    os_lock(&d.lock);
    memcpy(d.zero_pos, C.factory_zero, sizeof d.zero_pos);
    os_unlock(&d.lock);
    char *text = store_get(FILE_NAME, NULL);
    cJSON *c = text ? cJSON_Parse(text) : NULL;
    free(text);
    os_lock(&lock);
    const char *err = c ? apply(c) : "calib_missing";
    if (!err) printf("calibration: from data/" FILE_NAME "\n");
    else if (c) printf("calibration: data/" FILE_NAME ": %s\n", err);
    if (err) load_old();
    os_unlock(&lock);
    cJSON_Delete(c);
}

void calib_watch(void)
{
    static int level_result;
    os_lock(&d.lock);
    const int now = d.level_result;
    os_unlock(&d.lock);
    if (now == 2 && level_result != 2) {
        os_lock(&lock);
        steps[STEP_LEVEL] = 1;
        save();
        os_unlock(&lock);
    }
    level_result = now;
}

/* ---- methods ---- */

static const char *in_calibration(int src, int need_torque_off)
{
    os_lock(&d.lock);
    const char *err = d.mode != MODE_CALIBRATE ? "not_calibrating"
                    : need_torque_off && d.cal_hold ? "calib_holding" : arb_claim(src, os_now());
    os_unlock(&d.lock);
    return err;
}

/*
 * {hold}: with hold the servos keep calib.targets with a weak force, starting where they are so
 * nothing jumps; otherwise all torque goes off.
 */
static const char *calib_enter(int src, const cJSON *p, cJSON *r)
{
    (void)r;
    os_lock(&d.lock);
    const char *err = arb_set_mode(src, MODE_CALIBRATE, os_now());
    if (!err) d.cal_hold = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "hold"));
    if (!err && d.cal_hold) memcpy(d.cal_target, d.raw_pos, sizeof d.cal_target);
    os_unlock(&d.lock);
    return err;
}

static const char *calib_exit(int src, const cJSON *p, cJSON *r)
{
    (void)p;
    (void)r;
    os_lock(&d.lock);
    const char *err = arb_set_mode(src, MODE_PREVIEW, os_now());
    os_unlock(&d.lock);
    return err;
}

/* {targets}: where the holding servos go, raw 0..4095 each. */
static const char *calib_targets(int src, const cJSON *p, cJSON *r)
{
    (void)r;
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(p, "targets");
    if (cJSON_GetArraySize(a) != C.n) return "bad_targets";
    for (int i = 0; i < C.n; i++) {
        const cJSON *v = cJSON_GetArrayItem(a, i);
        if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 4095) return "bad_targets";
    }
    os_lock(&d.lock);
    const char *err = d.mode != MODE_CALIBRATE ? "not_calibrating" : !d.cal_hold ? "calib_not_holding" : arb_claim(src, os_now());
    for (int i = 0; i < C.n && !err; i++) d.cal_target[i] = (uint16_t)cJSON_GetArrayItem(a, i)->valueint;
    os_unlock(&d.lock);
    return err;
}

/* {index}: the joint's present position becomes its zero (turned there by the servos, step 3). */
static const char *calib_finish(int src, const cJSON *p, cJSON *r)
{
    const int i = (int)num(p, "index", -1);
    if (i < 0 || i >= C.n) return "bad_joint";
    const char *err = in_calibration(src, 0);
    if (err) return err;
    os_lock(&lock);
    os_lock(&d.lock);
    d.zero_pos[i] = d.raw_pos[i];
    d.cal_done[i] = 1;
    int done = 1;
    for (int k = 0; k < C.n; k++) done &= d.cal_done[k];
    cJSON_AddNumberToObject(r, "zero_pos", d.zero_pos[i]);
    os_unlock(&d.lock);
    if (done) steps[STEP_ZERO] = 1;
    err = save();
    os_unlock(&lock);
    return err;
}

/*
 * Step 3, {index} or nothing: the zeros from the calibration pose, one joint or every joint at
 * once, with how far each is from the factory zero. The step is done once every joint is recorded.
 */
static const char *calib_capture(int src, const cJSON *p, cJSON *r)
{
    const int one = (int)num(p, "index", -1);
    if (cJSON_GetObjectItemCaseSensitive(p, "index") && (one < 0 || one >= C.n)) return "bad_joint";
    const char *err = in_calibration(src, 1);
    if (err) return err;
    const cJSON *pose = cfg("pose_deg");
    const uint32_t need = one >= 0 ? 1u << one : (1u << C.n) - 1;
    uint16_t raw[MAX_MOTORS], zero[MAX_MOTORS];
    os_lock(&d.lock);
    const int online = (d.servo_mask & need) == need;
    memcpy(raw, d.raw_pos, sizeof raw);
    memcpy(zero, d.zero_pos, sizeof zero);
    os_unlock(&d.lock);
    if (!online) return "servos_offline";

    float dev[MAX_MOTORS];
    for (int i = 0; i < C.n; i++) {
        if (one < 0 || i == one) {
            const cJSON *v = cJSON_GetArrayItem(pose, i);
            zero[i] = clamp_raw(raw[i] - raw_from_deg(cJSON_IsNumber(v) ? v->valuedouble : 0));
        }
        dev[i] = (float)((zero[i] - C.factory_zero[i]) * 360.0 / 4095.0 * C.motor_sign);
    }
    os_lock(&lock);
    os_lock(&d.lock);
    int done = 1;
    for (int i = 0; i < C.n; i++) {
        if (!(need >> i & 1)) continue;
        d.zero_pos[i] = zero[i];
        d.cal_done[i] = 1;
    }
    for (int i = 0; i < C.n; i++) done &= d.cal_done[i];
    os_unlock(&d.lock);
    if (done) steps[STEP_ZERO] = 1;
    err = save();
    os_unlock(&lock);
    int z[MAX_MOTORS];
    for (int i = 0; i < C.n; i++) z[i] = zero[i];
    cJSON_AddItemToObject(r, "zero_pos", cJSON_CreateIntArray(z, C.n));
    cJSON_AddItemToObject(r, "deviation_deg", cJSON_CreateFloatArray(dev, C.n));
    return err;
}

/*
 * Step 5: the duck stands in the default pose (hold) on flat ground. Over a
 * second: torso roll and pitch, and each joint's tracking error. A roll over
 * 1 deg is answered with nudges from calibration.roll_fix, [[index, deg per
 * deg of roll], ...], which depend on the robot and are found by trying.
 */
static const char *calib_suggest(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)p;
    double roll = 0, pitch = 0, err[MAX_MOTORS] = {0};
    for (int k = 0; k < 10; k++) {
        os_lock(&d.lock);
        const int standing = d.mode == MODE_HOLD && d.enabled;
        const float gx = d.grav[0], gy = d.grav[1], gz = d.grav[2];
        for (int i = 0; i < C.n; i++) err[i] += d.target[i] - d.angle[i];
        os_unlock(&d.lock);
        if (!standing) return "calib_needs_hold";
        roll += atan2(-gy, -gz) * 180 / M_PI;
        pitch += atan2(gx, -gz) * 180 / M_PI;
        os_sleep(0.1);
    }
    float tracking[MAX_MOTORS];
    for (int i = 0; i < C.n; i++) tracking[i] = (float)(err[i] / 10);
    roll /= 10;
    pitch /= 10;
    cJSON_AddNumberToObject(r, "roll_deg", roll);
    cJSON_AddNumberToObject(r, "pitch_deg", pitch);
    cJSON_AddItemToObject(r, "tracking_deg", cJSON_CreateFloatArray(tracking, C.n));
    cJSON *nudges = cJSON_AddArrayToObject(r, "nudges");
    const cJSON *fix;
    cJSON_ArrayForEach(fix, cfg("roll_fix")) {
        const cJSON *i = cJSON_GetArrayItem(fix, 0), *k = cJSON_GetArrayItem(fix, 1);
        if (fabs(roll) <= 1 || !cJSON_IsNumber(i) || !cJSON_IsNumber(k)) continue;
        double delta = -k->valuedouble * roll;
        delta = delta > MAX_NUDGE_DEG ? MAX_NUDGE_DEG : delta < -MAX_NUDGE_DEG ? -MAX_NUDGE_DEG : delta;
        const double n[2] = {i->valueint, delta};
        cJSON_AddItemToArray(nudges, cJSON_CreateDoubleArray(n, 2));
    }
    return NULL;
}

/*
 * {index, delta} or {nudges: [[index, delta], ...]}: moves zeros by joint
 * degrees, at most 5 each, while the duck holds a pose.
 */
static const char *calib_nudge(int src, const cJSON *p, cJSON *r)
{
    int idx[MAX_MOTORS], n = 0;
    double delta[MAX_MOTORS];
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(p, "nudges"), *e;
    if (list) {
        cJSON_ArrayForEach(e, list) {
            if (n == MAX_MOTORS || cJSON_GetArraySize(e) != 2) return "bad_joint";
            idx[n] = cJSON_GetArrayItem(e, 0)->valueint;
            delta[n++] = cJSON_GetArrayItem(e, 1)->valuedouble;
        }
    } else {
        idx[n] = (int)num(p, "index", -1);
        delta[n++] = num(p, "delta", 0);
    }
    for (int k = 0; k < n; k++) {
        if (idx[k] < 0 || idx[k] >= C.n) return "bad_joint";
        if (!(fabs(delta[k]) <= MAX_NUDGE_DEG)) return "nudge_too_big";
    }
    os_lock(&lock);
    os_lock(&d.lock);
    const char *err = d.mode >= MODE_CALIBRATE ? "exit_setup_first" : arb_claim(src, os_now());
    for (int k = 0; k < n && !err; k++) d.zero_pos[idx[k]] = clamp_raw(d.zero_pos[idx[k]] + raw_from_deg(delta[k]));
    int zero[MAX_MOTORS];
    for (int i = 0; i < C.n; i++) zero[i] = d.zero_pos[i];
    os_unlock(&d.lock);
    if (!err) err = save();
    os_unlock(&lock);
    cJSON_AddItemToObject(r, "zero_pos", cJSON_CreateIntArray(zero, C.n));
    return err;
}

/* {cmd}: 1 measures the gyro bias on the IMU board, 2 levels the torso here, 3 as the IMU board defines. */
static const char *calib_imu(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)r;
    const int cmd = (int)num(p, "cmd", 0);
    if (cmd < 1 || cmd > 3) return "bad_imu_cmd";
    os_lock(&d.lock);
    /* Leveling may also stand in the default pose on flat ground: the symmetric joints hold the torso level. */
    const int ok = !d.enabled || (cmd == 2 && d.mode == MODE_HOLD);
    const char *err = ok ? NULL : cmd == 2 ? "level_needs_rest" : "torque_off_first";
    /* Leveling is measured here: the IMU board's own assumes its chip upright, not upside down as mounted. */
    if (!err && cmd == 2) {
        d.level_cmd = 1;
        d.level_result = 1;
    } else if (!err) {
        d.imu_cal_cmd = cmd;
    }
    os_unlock(&d.lock);
    return err;
}

/* {step, ok}: the web page records a step the user confirmed (servos, imu_bias, stand). */
static const char *calib_mark(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)r;
    const cJSON *step = cJSON_GetObjectItemCaseSensitive(p, "step");
    int k = NSTEPS;
    while (cJSON_IsString(step) && --k >= 0 && strcmp(step->valuestring, STEPS[k])) {}
    if (k < 0 || k >= NSTEPS) return "calib_step";
    os_lock(&lock);
    steps[k] = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p, "ok"));
    const char *err = save();
    os_unlock(&lock);
    return err;
}

/* The calibration now, as calibration.json holds it; the web page's first-use check reads its steps. */
static const char *calib_export(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)p;
    os_lock(&lock);
    cJSON_AddItemToObject(r, "calibration", snapshot());
    os_unlock(&lock);
    char *prev = store_get(PREV_NAME, NULL);
    cJSON_AddBoolToObject(r, "can_undo", prev != NULL);
    free(prev);
    return NULL;
}

/* {calibration}: an exported one, for the same robot (its servo ids). */
static const char *calib_import(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)r;
    os_lock(&d.lock);
    const char *err = d.enabled ? "torque_off_first" : NULL;
    os_unlock(&d.lock);
    if (err) return err;
    os_lock(&lock);
    cJSON *old = snapshot();
    err = apply(cJSON_GetObjectItemCaseSensitive(p, "calibration"));
    if (!err) err = save();
    else apply(old);
    os_unlock(&lock);
    cJSON_Delete(old);
    return err;
}

/* Back to the calibration before the last change; undoing again redoes it. */
static const char *calib_undo(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)p;
    (void)r;
    os_lock(&d.lock);
    const char *err = d.enabled ? "torque_off_first" : NULL;
    os_unlock(&d.lock);
    if (err) return err;
    os_lock(&lock);
    char *prev = store_get(PREV_NAME, NULL), *cur = store_get(FILE_NAME, NULL);
    cJSON *c = prev ? cJSON_Parse(prev) : NULL;
    err = !c ? "calib_no_undo" : apply(c);
    if (!err && store_put(FILE_NAME, prev, strlen(prev)) == 0 && cur) store_put(PREV_NAME, cur, strlen(cur));
    os_unlock(&lock);
    cJSON_Delete(c);
    free(prev);
    free(cur);
    return err;
}

const struct api_method CALIB_METHODS[] = {
    {"calib.enter", calib_enter, 1},     {"calib.exit", calib_exit, 1},       {"calib.targets", calib_targets, 1},
    {"calib.finish", calib_finish, 1},   {"calib.capture", calib_capture, 1},
    {"calib.suggest", calib_suggest, 1}, {"calib.nudge", calib_nudge, 1},     {"calib.imu", calib_imu, 1},
    {"calib.mark", calib_mark, 1},       {"calib.export", calib_export, 1},   {"calib.import", calib_import, 1},
    {"calib.undo", calib_undo, 1},
};
const int NCALIB_METHODS = sizeof CALIB_METHODS / sizeof CALIB_METHODS[0];
