/*
 * Loads the board and robot files into B and C. A missing or malformed field
 * stops the program with the field's name: a robot half-described is not safe
 * to power.
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "duck.h"
#include "storage.h"

/* A build with no Kconfig, the PC: no bus, no audio. */
#ifndef CONFIG_DOITDUCK_BOARD
#define CONFIG_DOITDUCK_BOARD "pc"
#define CONFIG_DOITDUCK_ROBOT "microduck"
#define CONFIG_DOITDUCK_HTTP_PORT 9527
#define CONFIG_DOITDUCK_BUS_DEVICE ""
#define CONFIG_DOITDUCK_BUS_PINMUX ""
#define CONFIG_DOITDUCK_BUS_TX_ENABLE_BASE 0
#define CONFIG_DOITDUCK_BUS_TX_ENABLE_PIN (-1)
#define CONFIG_DOITDUCK_MIC_CHANNEL 0
#define CONFIG_DOITDUCK_MIC_GAIN_DB 50
#define CONFIG_DOITDUCK_SPEAKER_GAIN_DB (-10)
#endif

struct board_cfg B;
static cJSON *robot_root;

static cJSON *load(const char *dir, const char *name)
{
    char path[96];
    size_t size = 0;
    snprintf(path, sizeof path, "%s/%s.json", dir, name);
    const char *text = store_blob(path, &size);
    if (!text) {
        printf("config: cannot read %s\n", path);
        return NULL;
    }
    cJSON *root = cJSON_Parse(text);
    if (!root) printf("config: %s is not valid JSON near: %.40s\n", path, cJSON_GetErrorPtr());
    return root;
}

/* Reads a number array of exactly n elements; prints and returns -1 otherwise. */
static int numbers(const cJSON *obj, const char *key, float *out, int n)
{
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsArray(a) || cJSON_GetArraySize(a) != n) {
        printf("config: \"%s\" needs %d numbers\n", key, n);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        const cJSON *v = cJSON_GetArrayItem(a, i);
        if (!cJSON_IsNumber(v)) {
            printf("config: \"%s\"[%d] is not a number\n", key, i);
            return -1;
        }
        out[i] = (float)v->valuedouble;
    }
    return 0;
}

static double number(const cJSON *obj, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

static int motor_index(int id)
{
    for (int i = 0; i < C.n; i++) {
        if (C.id[i] == id) return i;
    }
    return -1;
}

void config_load_board(void)
{
    snprintf(B.name, sizeof B.name, "%s", CONFIG_DOITDUCK_BOARD);
    snprintf(B.robot, sizeof B.robot, "%s", CONFIG_DOITDUCK_ROBOT);
    B.http_port = CONFIG_DOITDUCK_HTTP_PORT;
    B.audio.mic_channel = CONFIG_DOITDUCK_MIC_CHANNEL;
    B.audio.mic_gain_db = CONFIG_DOITDUCK_MIC_GAIN_DB;
    B.audio.speaker_gain_db = CONFIG_DOITDUCK_SPEAKER_GAIN_DB;
    snprintf(B.bus.device, sizeof B.bus.device, "%s", CONFIG_DOITDUCK_BUS_DEVICE);
    B.bus.tx_enable_base = CONFIG_DOITDUCK_BUS_TX_ENABLE_BASE;
    B.bus.tx_enable_pin = CONFIG_DOITDUCK_BUS_TX_ENABLE_PIN;
    /* "reg=value reg=value ..." */
    const char *p = CONFIG_DOITDUCK_BUS_PINMUX;
    char *end;
    while (B.bus.npinmux < 8) {
        const uint32_t reg = (uint32_t)strtoul(p, &end, 16);
        if (end == p || *end != '=') break;
        B.bus.pinmux[B.bus.npinmux][0] = reg;
        B.bus.pinmux[B.bus.npinmux++][1] = (uint32_t)strtoul(end + 1, &end, 16);
        p = end;
    }
}

int config_load_robot(const char *name)
{
    robot_root = load("robots", name);
    if (!robot_root) return -1;
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(robot_root, "motors");
    const cJSON *pol = cJSON_GetObjectItemCaseSensitive(robot_root, "policy");
    const cJSON *imu = cJSON_GetObjectItemCaseSensitive(robot_root, "imu");
    const cJSON *lim = cJSON_GetObjectItemCaseSensitive(robot_root, "limits");
    float v[MAX_MOTORS];

    snprintf(C.name, sizeof C.name, "%s", name);
    C.n = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(m, "ids"));
    if (C.n < 1 || C.n > MAX_MOTORS || numbers(m, "ids", v, C.n)) return -1;
    for (int i = 0; i < C.n; i++) C.id[i] = (uint8_t)v[i];
    if (numbers(m, "home_deg", C.home, C.n) || numbers(m, "factory_zero", v, C.n)) return -1;
    for (int i = 0; i < C.n; i++) C.factory_zero[i] = (uint16_t)v[i];
    C.motor_sign = (float)number(m, "sign", -1);
    C.mouth = motor_index((int)number(m, "mouth_id", -1));
    C.mouth_max = (float)number(m, "mouth_max_deg", 0);
    C.kp = (uint8_t)number(m, "kp", 6);
    C.kd = (uint8_t)number(m, "kd", 20);
    C.mouth_kp = (uint8_t)number(m, "mouth_kp", C.kp);
    C.cal_kp = (uint8_t)number(m, "calibrate_kp", 3);
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(m, "kp_by_id")) {
        const int i = motor_index(atoi(e->string));
        if (i < 0 || !cJSON_IsNumber(e)) {
            printf("config: \"kp_by_id\" \"%s\" is not a motor with a number\n", e->string);
            return -1;
        }
        C.motor_kp[i] = (uint8_t)e->valuedouble;
    }

    float im[3];
    if (numbers(imu, "axis_map", im, 3) || numbers(imu, "axis_sign", C.imu_sign, 3)) return -1;
    for (int k = 0; k < 3; k++) C.imu_map[k] = (int)im[k];

    float act[ACT_DIM], head[2];
    if (numbers(pol, "action_ids", act, ACT_DIM) || numbers(pol, "head_actions", head, 2)) return -1;
    for (int i = 0; i < ACT_DIM; i++) {
        C.act_motor[i] = motor_index((int)act[i]);
        if (C.act_motor[i] < 0) {
            printf("config: action_ids[%d] = %d is not a motor\n", i, (int)act[i]);
            return -1;
        }
    }
    C.head_act_begin = (int)head[0];
    C.head_act_end = (int)head[1];
    const cJSON *fall = cJSON_GetObjectItemCaseSensitive(pol, "fall");
    const cJSON *stand = cJSON_GetObjectItemCaseSensitive(pol, "stand");
    C.fall_deg = (float)number(fall, "tilt_deg", 55);
    C.fall_s = (float)number(fall, "seconds", 0.15);
    C.hold_s = (float)number(fall, "hold_s", 1.0);
    C.stand_deg = (float)number(stand, "tilt_deg", 15);
    C.stand_s = (float)number(stand, "seconds", 1.0);

    for (int s = SRC_PAD; s < SRC_COUNT; s++) {
        if (numbers(lim, SOURCE_NAMES[s], C.vmax[s], 3)) return -1;
    }
    C.low_battery_mv = (int)(number(robot_root, "low_battery_v", 6.6) * 1000);
    d.alpha_leg = d.alpha_head = (float)number(pol, "smoothing", 0.45);
    d.alpha_head = (float)number(pol, "smoothing_head", d.alpha_head);
    return 0;
}

const cJSON *config_section(const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(robot_root, key);
}

const cJSON *config_robot(void)
{
    return robot_root;
}
