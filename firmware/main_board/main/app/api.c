/*
 * Methods of the command interface. Each takes the source, its params and an
 * empty result object; it returns NULL or an error code. Motion goes through
 * the arbiter; calibration, servo setup and configuration are web-only. The
 * features add their own methods with api_register.
 */
#include "api.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "config.h"
#include "duck.h"
#include "policies.h"

#define MAX_METHODS 64

static const struct {
    const char *code, *message;
} ERRORS[] = {
    {"busy_pad", "the gamepad has control"},
    {"busy_web", "the web page has control"},
    {"busy_voice", "voice has control"},
    {"busy_llm", "the LLM has control"},
    {"busy_gesture", "gestures have control"},
    {"setup_web_only", "calibration and servo setup start from the web page only"},
    {"exit_setup_first", "leave calibration or servo setup first"},
    {"torque_off_first", "turn torque off first"},
    {"no_feedback", "no feedback from the servos or the IMU"},
    {"imu_offline", "the IMU is offline"},
    {"battery_low", "battery low, charge it before turning torque on"},
    {"in_setup", "no motion during calibration or servo setup"},
    {"skill_running", "a skill is running"},
    {"duration_required", "a move needs a duration"},
    {"bad_velocity", "the velocity is not a number"},
    {"bad_head", "head needs 4 numbers"},
    {"bad_mouth", "mouth angle out of range"},
    {"no_such_skill", "no such skill"},
    {"skill_needs_walk", "skills run in walk or preview mode"},
    {"skill_not_now", "skills run only while standing, with no other skill running"},
    {"setup_heartbeat_lost", "calibration or servo setup lost its heartbeat and was left"},
    {"feedback_lost", "servo or IMU feedback lost, torque off"},
    {"policy_not_finite", "the policy output NaN or Inf, torque off"},
    {"unknown_mode", "unknown mode"},
    {"bad_filter", "smoothing must be between 0 and 1"},
    {"bad_targets", "one target per servo, 0 to 4095"},
    {"not_calibrating", "not in calibration"},
    {"bad_joint", "joint index out of range"},
    {"nudge_too_big", "at most 5 degrees per nudge"},
    {"bad_imu_cmd", "the IMU command is 1, 2 or 3"},
    {"level_needs_rest", "leveling needs torque off, or standing in the default pose on flat ground"},
    {"not_in_servo_mode", "not in servo setup"},
    {"unknown_servo_op", "unknown servo operation"},
    {"bad_id", "ids are 0 to 253"},
    {"bad_gains", "KP and KD are 0 to 255"},
    {"bad_position", "position out of range"},
    {"id_no_reply", "no servo answers this id; id not changed"},
    {"id_taken", "the new id is taken"},
    {"id_new_no_reply", "new id written, but it does not answer; scan again"},
    {"id_both_reply", "old and new id both answer; connect one servo only and scan again"},
    {"servo_op_failed", "the servo operation failed"},
    {"web_only", "this method is for the web page only"},
    {"wifi_ssid", "a network name is 1 to 32 bytes"},
    {"wifi_password", "a password is 8 to 63 plain characters, or none for an open network"},
    {"wifi_write", "could not save the network or start Wi-Fi"},
    {"unknown_method", "unknown method"},
    {"wake_first", "say the wake word first"},
    {"unknown_words", "no command for these words"},
    {"no_base_policies", "no walk or get-up policy; import them first"},
    {"policy_not_verified", "this policy is not verified yet; run it from the web page first"},
    {"policy_name", "a policy name is letters, digits, _ and -, and unique"},
    {"policy_manifest", "the manifest is missing or not valid JSON"},
    {"policy_model_api", "the policy needs another model_api"},
    {"policy_obs_len", "the policy's obs_len is not 61"},
    {"policy_action_len", "the policy's action_len is not 14"},
    {"policy_action_scale", "action_scale other than 1 is not supported"},
    {"policy_robot_model", "the policy is for another robot (robot.model not in compatible_models)"},
    {"policy_kind", "this kind and command encoding are not supported"},
    {"policy_home", "doitduck.home_deg needs one number per motor"},
    {"policy_duration", "the policy needs duration_s or command.period_s"},
    {"policy_size", "the weights are not a 61-512-256-128-14 network"},
    {"policy_in_use", "the policy is in use; turn torque off or let it finish"},
    {"policy_replace_base", "this replaces the walk or get-up policy; confirm it"},
    {"policy_full", "no room for more policies"},
    {"policy_storage", "cannot save the policy"},
    {"policy_not_found", "no such policy"},
    {"policy_builtin", "built-in policies cannot be removed"},
    {"no_such_sound", "no such sound"},
    {"sound_too_often", "at most one sound a second from each source"},
    {"sound_busy", "another sound is playing"},
    {"no_audio", "no speaker"},
    {"sound_name", "a sound name is letters, digits, _ and -"},
    {"sound_format", "a sound is a 16 kHz mono 16-bit WAV of at most 3 s"},
    {"sound_full", "no room for more sounds"},
    {"sound_storage", "cannot save the sound"},
    {"sound_builtin", "built-in sounds cannot be removed"},
    {"bad_volume", "volume_db is -40 to 10"},
    {"servos_offline", "not every servo answers"},
    {"calib_holding", "calibration is holding the servos; enter it without hold"},
    {"calib_not_holding", "calibration was entered without hold"},
    {"calib_needs_hold", "stand the duck in the default pose (hold) on flat ground first"},
    {"calib_step", "unknown calibration step"},
    {"calib_mismatch", "the calibration is for another robot or is damaged"},
    {"calib_storage", "cannot save the calibration"},
    {"calib_no_undo", "no earlier calibration"},
    {"calib_missing", "no calibration yet"},
};

static const struct api_method *extra[MAX_METHODS];
static int nextra;

static double num(const cJSON *p, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(p, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

static const char *str(const cJSON *p, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(p, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

/* An array element's value, NAN when it is not a number. */
static double item(const cJSON *a, int i)
{
    const cJSON *v = cJSON_GetArrayItem(a, i);
    return cJSON_IsNumber(v) ? v->valuedouble : NAN;
}

static void add_floats(cJSON *r, const char *key, const float *v, int n)
{
    cJSON_AddItemToObject(r, key, cJSON_CreateFloatArray(v, n));
}

static void add_ints(cJSON *r, const char *key, const int *v, int n)
{
    cJSON_AddItemToObject(r, key, cJSON_CreateIntArray(v, n));
}

static void add_motor_u16(cJSON *r, const char *key, const uint16_t *v)
{
    int x[MAX_MOTORS];
    for (int i = 0; i < C.n; i++) x[i] = v[i];
    add_ints(r, key, x, C.n);
}

/* ---- robot ---- */

static const char *robot_info(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)p;
    int ids[MAX_MOTORS];
    for (int i = 0; i < C.n; i++) ids[i] = C.id[i];
    cJSON_AddStringToObject(r, "name", C.name);
    add_ints(r, "motors", ids, C.n);
    const cJSON *names = cJSON_GetObjectItemCaseSensitive(config_section("motors"), "names");
    if (names) cJSON_AddItemToObject(r, "motor_names", cJSON_Duplicate(names, 1));
    policies_skills(cJSON_AddArrayToObject(r, "skills"));
    cJSON_AddNumberToObject(r, "mouth_max", C.mouth_max);
    add_floats(r, "vmax", C.vmax[SRC_WEB], 3);
    app_features(r, "features");
    cJSON_AddStringToObject(r, "board", B.name);
    cJSON_AddStringToObject(r, "version", DOITDUCK_VERSION);
    int factory[MAX_MOTORS];
    for (int i = 0; i < C.n; i++) factory[i] = C.factory_zero[i];
    add_ints(r, "factory_zero", factory, C.n);
    cJSON_AddNumberToObject(r, "motor_sign", C.motor_sign);
    const cJSON *calibration = config_section("calibration");
    if (calibration) cJSON_AddItemToObject(r, "calibration", cJSON_Duplicate(calibration, 1));
    const cJSON *diagram = config_section("diagram");
    if (diagram) cJSON_AddItemToObject(r, "diagram", cJSON_Duplicate(diagram, 1));
    return NULL;
}

static const char *robot_status(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)p;
    static const char *const phases[] = {"walk", "default_pose", "getup"};
    int online[MAX_MOTORS], n = 0;
    const double now = os_now();
    os_lock(&d.lock);
    const int fresh = d.have_fb && now - d.rx_time < 0.15;
    cJSON_AddStringToObject(r, "mode", MODE_NAMES[d.mode]);
    cJSON_AddStringToObject(r, "owner", SOURCE_NAMES[d.owner]);
    cJSON_AddBoolToObject(r, "enabled", d.enabled);
    cJSON_AddBoolToObject(r, "imu_ok", d.imu_ok);
    cJSON_AddBoolToObject(r, "timed_out", d.timed_out);
    cJSON_AddBoolToObject(r, "control_ready", fresh && now - d.imu_time < 0.1 && d.mode < MODE_CALIBRATE);
    cJSON_AddStringToObject(r, "last_error_code", d.last_error ? d.last_error : "");
    cJSON_AddNumberToObject(r, "feedback_hz", d.feedback_hz);
    cJSON_AddNumberToObject(r, "inference_hz", d.inference_hz);
    cJSON_AddNumberToObject(r, "inference_ms", d.inference_ms);
    cJSON_AddNumberToObject(r, "bus_work_us", d.bus_work_us);
    cJSON_AddNumberToObject(r, "bus_overruns", d.bus_overruns);
    cJSON_AddNumberToObject(r, "imu_misses", d.imu_misses);
    if (d.have_fb) cJSON_AddNumberToObject(r, "feedback_age_ms", (now - d.rx_time) * 1000);
    else cJSON_AddNullToObject(r, "feedback_age_ms");
    cJSON_AddNumberToObject(r, "battery_v", d.battery_mv / 1000.0);
    cJSON_AddNumberToObject(r, "imu_status", d.imu_status);
    cJSON_AddNumberToObject(r, "imu_cal_result", d.imu_cal_result);
    add_floats(r, "imu_bias", d.imu_bias, 3);
    add_floats(r, "imu_mount", d.imu_mount, 2);
    add_floats(r, "imu_level", d.imu_level, 2);
    cJSON_AddNumberToObject(r, "level_result", d.level_result);
    cJSON_AddStringToObject(r, "phase", phases[d.phase]);
    if (d.tilt_valid) cJSON_AddNumberToObject(r, "tilt_deg", d.tilt_deg);
    else cJSON_AddNullToObject(r, "tilt_deg");
    cJSON_AddNumberToObject(r, "fall_elapsed", d.fall_elapsed);
    cJSON_AddNumberToObject(r, "stand_elapsed", d.stand_elapsed);
    if (d.skill >= 0) cJSON_AddStringToObject(r, "skill", C.p[d.skill].name);
    else cJSON_AddNullToObject(r, "skill");
    cJSON_AddNumberToObject(r, "skill_progress", d.skill_phi);
    for (int i = 0; i < C.n; i++) {
        if (d.servo_mask & (1u << i)) online[n++] = C.id[i];
    }
    add_ints(r, "online", online, n);
    add_floats(r, "angles", d.angle, C.n);
    add_floats(r, "targets", d.target, C.n);
    add_floats(r, "velocities", d.vel, C.n);
    add_motor_u16(r, "raw_pos", d.raw_pos);
    add_motor_u16(r, "zero_pos", d.zero_pos);
    add_motor_u16(r, "cal_targets", d.cal_target);
    add_ints(r, "cal_done", d.cal_done, C.n);
    add_floats(r, "acc", d.acc, 3);
    add_floats(r, "gyro", d.gyro, 3);
    add_floats(r, "gravity", d.grav, 3);
    add_floats(r, "twist", d.twist, 3);
    add_floats(r, "head", d.head, 4);
    cJSON_AddNumberToObject(r, "mouth", d.mouth);
    cJSON_AddNumberToObject(r, "alpha_leg", d.alpha_leg);
    cJSON_AddNumberToObject(r, "alpha_head", d.alpha_head);
    os_unlock(&d.lock);
    app_status(r);
    return NULL;
}

static const char *robot_set_mode(int src, const cJSON *p, cJSON *r)
{
    (void)r;
    const char *name = str(p, "mode");
    for (int m = 0; m < MODE_COUNT; m++) {
        if (strcmp(name, MODE_NAMES[m])) continue;
        os_lock(&d.lock);
        const char *err = arb_set_mode(src, m, os_now());
        os_unlock(&d.lock);
        return err;
    }
    return "unknown_mode";
}

static const char *robot_move(int src, const cJSON *p, cJSON *r)
{
    (void)r;
    const float t[3] = {(float)num(p, "vx", 0), (float)num(p, "vy", 0), (float)num(p, "wz", 0)};
    os_lock(&d.lock);
    const char *err = arb_move(src, t, num(p, "duration", 0), os_now());
    os_unlock(&d.lock);
    return err;
}

static const char *robot_stop(int src, const cJSON *p, cJSON *r)
{
    (void)p;
    (void)r;
    os_lock(&d.lock);
    arb_stop(src, os_now());
    os_unlock(&d.lock);
    return NULL;
}

static const char *robot_estop(int src, const cJSON *p, cJSON *r)
{
    (void)p;
    (void)r;
    os_lock(&d.lock);
    arb_estop(src, os_now());
    os_unlock(&d.lock);
    return NULL;
}

static const char *robot_skill(int src, const cJSON *p, cJSON *r)
{
    (void)r;
    os_lock(&d.lock);
    const char *err = arb_skill(src, str(p, "name"), os_now());
    os_unlock(&d.lock);
    return err;
}

static const char *robot_head(int src, const cJSON *p, cJSON *r)
{
    (void)r;
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(p, "head");
    float h[4];
    if (cJSON_GetArraySize(a) != 4) return "bad_head";
    for (int k = 0; k < 4; k++) h[k] = (float)item(a, k);
    os_lock(&d.lock);
    const char *err = arb_head(src, h, os_now());
    os_unlock(&d.lock);
    return err;
}

static const char *robot_mouth(int src, const cJSON *p, cJSON *r)
{
    (void)r;
    os_lock(&d.lock);
    const char *err = arb_mouth(src, (float)num(p, "deg", -1), os_now());
    os_unlock(&d.lock);
    return err;
}

static const char *robot_filter(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    os_lock(&d.lock);
    const double leg = num(p, "leg", d.alpha_leg), head = num(p, "head", d.alpha_head);
    const char *err = leg >= 0 && leg <= 1 && head >= 0 && head <= 1 ? NULL : "bad_filter";
    if (!err) {
        d.alpha_leg = (float)leg;
        d.alpha_head = (float)head;
    }
    cJSON_AddNumberToObject(r, "leg", d.alpha_leg);
    cJSON_AddNumberToObject(r, "head", d.alpha_head);
    os_unlock(&d.lock);
    return err;
}

/* ---- calibration and servo setup ---- */

static const char *enter_mode(int src, int mode)
{
    os_lock(&d.lock);
    const char *err = arb_set_mode(src, mode, os_now());
    os_unlock(&d.lock);
    return err;
}

static const char *setup_exit(int src, const cJSON *p, cJSON *r)
{
    (void)p;
    (void)r;
    return enter_mode(src, MODE_PREVIEW);
}

static const char *servo_enter(int src, const cJSON *p, cJSON *r)
{
    (void)p;
    (void)r;
    return enter_mode(src, MODE_SERVO);
}

static const char *servo_run(int src, const cJSON *p, cJSON *r)
{
    static const char *const actions[] = {"torque_off", "", "unlock", "set_id", "goto", "set_gains", "read", "ping"};
    const char *name = str(p, "action");
    int op = -1, raw = 0, ok = 1;
    for (int i = 0; i < 8; i++) {
        if (name[0] && !strcmp(name, actions[i])) op = i;
    }
    const double id = num(p, "target_id", -1), new_id = num(p, "new_id", 0);
    const double kp = num(p, "kp", 5), kd = num(p, "kd", 20), raw_pos = num(p, "raw_pos", 2047);
    os_lock(&d.lock);
    const char *err = d.mode != MODE_SERVO ? "not_in_servo_mode" : arb_claim(src, os_now());
    os_unlock(&d.lock);
    if (err) return err;
    if (op < 0) return "unknown_servo_op";
    if (!(id >= 0 && id <= 253) || !(new_id >= 0 && new_id <= 253)) return "bad_id";
    if (!(kp >= 0 && kp <= 255) || !(kd >= 0 && kd <= 255)) return "bad_gains";
    if (!(raw_pos >= 0 && raw_pos <= 4095)) return "bad_position";
    if (op == SERVO_SET_ID) {
        /* Check, write and verify, so a duplicate or missing ID is reported. */
        if (!servo_op(SERVO_PING, (int)id, 0, 0, NULL)) return "id_no_reply";
        if (id != new_id && servo_op(SERVO_PING, (int)new_id, 0, 0, NULL)) return "id_taken";
        if (id != new_id) {
            servo_op(SERVO_SET_ID, (int)id, (int)new_id, 0, NULL);
            os_sleep(0.05);
            if (!servo_op(SERVO_PING, (int)new_id, 0, 0, NULL)) return "id_new_no_reply";
            if (servo_op(SERVO_PING, (int)id, 0, 0, NULL)) return "id_both_reply";
        }
    } else {
        ok = servo_op(op, (int)id, op == SERVO_GOTO ? (int)raw_pos : (int)kp, (int)kd, &raw);
        if (!ok && op != SERVO_PING) return "servo_op_failed";
    }
    cJSON_AddBoolToObject(r, "ok", ok);
    cJSON_AddNumberToObject(r, "raw_pos", raw);
    return NULL;
}

/* ---- configuration, input sources ---- */

static const char *config_get(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)p;
    cJSON_AddItemToObject(r, "robot", cJSON_Duplicate(config_robot(), 1));
    cJSON_AddStringToObject(r, "board", B.name);
    cJSON_AddStringToObject(r, "version", DOITDUCK_VERSION);
    int factory[MAX_MOTORS];
    for (int i = 0; i < C.n; i++) factory[i] = C.factory_zero[i];
    add_ints(r, "factory_zero", factory, C.n);
    cJSON_AddNumberToObject(r, "motor_sign", C.motor_sign);
    const cJSON *calibration = config_section("calibration");
    if (calibration) cJSON_AddItemToObject(r, "calibration", cJSON_Duplicate(calibration, 1));
    return NULL;
}

static const struct api_method METHODS[] = {
    {"robot.info", robot_info, 0},
    {"robot.status", robot_status, 0},
    {"robot.set_mode", robot_set_mode, 0},
    {"robot.move", robot_move, 0},
    {"robot.stop", robot_stop, 0},
    {"robot.estop", robot_estop, 0},
    {"robot.skill", robot_skill, 0},
    {"robot.head", robot_head, 0},
    {"robot.mouth", robot_mouth, 0},
    {"robot.filter", robot_filter, 1},
    {"servo.enter", servo_enter, 1},
    {"servo.exit", setup_exit, 1},
    {"servo.op", servo_run, 1},
    {"config.get", config_get, 1},
};

void api_register(const struct api_method *methods, int n)
{
    for (int i = 0; i < n && nextra < MAX_METHODS; i++) extra[nextra++] = &methods[i];
}

static const struct api_method *find(const char *name)
{
    for (size_t i = 0; i < sizeof METHODS / sizeof METHODS[0]; i++) {
        if (!strcmp(name, METHODS[i].name)) return &METHODS[i];
    }
    for (int i = 0; i < nextra; i++) {
        if (!strcmp(name, extra[i]->name)) return extra[i];
    }
    return NULL;
}

cJSON *api_call(int src, const char *method, const cJSON *params, const char **err)
{
    const struct api_method *m = find(method);
    if (!m) {
        *err = "unknown_method";
        return NULL;
    }
    if (m->web_only && src != SRC_WEB) {
        *err = "web_only";
        return NULL;
    }
    cJSON *r = cJSON_CreateObject();
    *err = m->fn(src, params, r);
    if (*err) {
        cJSON_Delete(r);
        return NULL;
    }
    return r;
}

int api_good_name(const char *s, size_t size)
{
    const size_t n = strlen(s);
    if (n == 0 || n >= size) return 0;
    for (size_t i = 0; i < n; i++) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

int api_base64_decode(const char *in, uint8_t *out, size_t size)
{
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (; *in && *in != '='; in++) {
        const char c = *in;
        const int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
                    : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (v < 0) return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n == size) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)n;
}

const char *api_message(const char *code)
{
    for (size_t i = 0; i < sizeof ERRORS / sizeof ERRORS[0]; i++) {
        if (!strcmp(code, ERRORS[i].code)) return ERRORS[i].message;
    }
    return code;
}

void api_web_closed(void)
{
    os_lock(&d.lock);
    if (d.owner == SRC_WEB && d.mode < MODE_CALIBRATE) {
        arb_stop(SRC_WEB, os_now());
        d.owner = SRC_NONE;
    }
    os_unlock(&d.lock);
}

int api_moving(void)
{
    os_lock(&d.lock);
    const int m = d.move_until > os_now() || d.skill >= 0;
    os_unlock(&d.lock);
    return m;
}
