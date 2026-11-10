/*
 * 100 Hz hardware loop: the SCS servo bus at 1 Mbps, the IMU board on the same
 * bus (ID 200, firmware/imu_board/README.md), the torque watchdog, and the IMU
 * filter. Motor ids, gains and axes come from the robot config C.
 */
#include "bus.h"
#include "duck.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define ADDR_ID 0x05
#define ADDR_PERM_KP_KD 0x15
#define ADDR_TORQUE 40
#define ADDR_GOAL_POS 0x2A
#define ADDR_TEMP_KP 50
#define ADDR_TEMP_KD 51
#define ADDR_UNLOCK 0x37
#define ADDR_PRESENT 0x38
#define PRESENT_LEN 6
#define TICK 0.01
/* Replies are awaited from the write() call; the request itself takes up to 1.5 ms on the wire. */
#define REPLY_WAIT 0.008
#define IMU_ID 200
/* Accel mg, gyro 0.1 deg/s, sample count, status: read every tick. */
#define IMU_FAST_ADDR 0x40
#define IMU_FAST_LEN 15
/* Gyro bias, battery, calibration command and result, mount roll/pitch: read once per second. */
#define IMU_SLOW_ADDR 0x56
#define IMU_SLOW_LEN 16
#define IMU_CAL_CMD 0x60

static int imu_ok;
static float acc[3], gyro[3];

#define LEVEL_SAMPLES 200 /* 2 s */

/* The leveling rotation, as on the IMU board: about x by roll, then about y by pitch. */
static void level(float v[3], const float deg[2])
{
    const float r = deg[0] * (float)M_PI / 180, p = deg[1] * (float)M_PI / 180;
    const float x = v[0], y = cosf(r) * v[1] - sinf(r) * v[2], z = sinf(r) * v[1] + cosf(r) * v[2];
    v[0] = cosf(p) * x + sinf(p) * z;
    v[1] = y;
    v[2] = -sinf(p) * x + cosf(p) * z;
}

/* Reply parser state; only touched with d.bus_lock held. */
static uint8_t rx_buf[32];
static int rx_state, rx_len, rx_data_len, rx_done;
static uint32_t rx_mask, all_mask;
static uint16_t pos_raw[MAX_MOTORS];
static int16_t vel_raw[MAX_MOTORS];
static int ping_id = -1, ping_seen, read_id = -1, read_seen, read_pos;
static uint8_t imu_rx[IMU_FAST_LEN + 1];
static int imu_want = -1, imu_seen;

static void scs_write(uint8_t id, uint8_t fun, const uint8_t *data, int len)
{
    uint8_t buf[8 + 2 + MAX_MOTORS * 7];
    uint8_t sum = (uint8_t)(id + len + 2 + fun);
    buf[0] = buf[1] = 0xFF;
    buf[2] = id;
    buf[3] = (uint8_t)(len + 2);
    buf[4] = fun;
    for (int i = 0; i < len; i++) {
        buf[5 + i] = data[i];
        sum += data[i];
    }
    buf[5 + len] = (uint8_t)~sum;
    bus_write(buf, len + 6);
}

static void write_byte(uint8_t id, uint8_t addr, uint8_t value)
{
    const uint8_t data[2] = {addr, value};
    scs_write(id, 0x03, data, 2);
}

static void set_torque(int on)
{
    write_byte(0xFE, ADDR_TORQUE, (uint8_t)on);
}

static void set_temp_kp_kd(uint8_t kp, uint8_t kd)
{
    write_byte(0xFE, ADDR_TEMP_KP, kp);
    write_byte(0xFE, ADDR_TEMP_KD, kd);
}

static void set_run_kp_kd(void)
{
    set_temp_kp_kd(C.kp, C.kd);
    /* A servo answers a write to it alone: let the answer pass before the next write. */
    if (C.mouth >= 0) {
        write_byte(C.id[C.mouth], ADDR_TEMP_KP, C.mouth_kp);
        os_sleep(0.002);
    }
    for (int i = 0; i < C.n; i++) {
        if (!C.motor_kp[i]) continue;
        write_byte(C.id[i], ADDR_TEMP_KP, C.motor_kp[i]);
        os_sleep(0.002);
    }
}

static uint16_t sat_pos(int v)
{
    return (uint16_t)(v < 0 ? 0 : v > 4095 ? 4095 : v);
}

static void sync_write_goals(const uint16_t *pos)
{
    uint8_t data[2 + MAX_MOTORS * 7] = {ADDR_GOAL_POS, 6};
    for (int i = 0; i < C.n; i++) {
        uint8_t *p = data + 2 + i * 7;
        p[0] = C.id[i];
        p[1] = (uint8_t)(pos[i] & 0xFF);
        p[2] = (uint8_t)(pos[i] >> 8);
        p[3] = p[4] = p[5] = p[6] = 0;
    }
    scs_write(0xFE, 0x83, data, 2 + C.n * 7);
}

static void parse_byte(uint8_t b)
{
    switch (rx_state) {
    case 0:
        rx_state = b == 0xFF ? 1 : 0;
        break;
    case 1:
        rx_state = b == 0xFF ? 2 : 0;
        break;
    case 2:
        rx_buf[2] = b;
        rx_state = 3;
        break;
    case 3:
        rx_buf[3] = b;
        if (b == 0x02 || b == 0x04 || b == 0x08 || b == 0x09 || b == 0x0B || b == IMU_FAST_LEN + 2 ||
            b == IMU_SLOW_LEN + 2) {
            rx_state = 4;
            rx_len = 0;
            rx_data_len = b;
        } else {
            rx_state = 0;
        }
        break;
    case 4: {
        rx_buf[4 + rx_len++] = b;
        if (rx_len < rx_data_len) break;
        rx_state = 0;
        uint8_t sum = 0;
        for (int i = 0; i < 1 + rx_data_len; i++) sum += rx_buf[2 + i];
        sum = (uint8_t)~sum;
        if (sum != rx_buf[3 + rx_data_len]) break;
        const int sid = rx_buf[2];
        if (rx_data_len == 2 && sid == ping_id) {
            /* Even a device error is evidence that this address responded. */
            ping_seen = rx_done = 1;
            break;
        }
        if (sid == IMU_ID) {
            if (rx_buf[4] == 0 && rx_data_len - 2 == imu_want) {
                memcpy(imu_rx, rx_buf + 5, (size_t)imu_want);
                imu_seen = rx_done = 1;
            }
            break;
        }
        /* Only accept the requested 6-byte status, with no device error. */
        if (rx_data_len != 8 || rx_buf[4] != 0) break;
        const uint16_t pos = (uint16_t)(rx_buf[5] | rx_buf[6] << 8);
        /* Present speed: bit 15 is direction, bits 0-14 are magnitude. */
        const uint16_t speed = (uint16_t)(rx_buf[7] | rx_buf[8] << 8);
        if (sid == read_id) {
            read_seen = rx_done = 1;
            read_pos = pos;
        }
        for (int i = 0; i < C.n; i++) {
            if (C.id[i] != sid) continue;
            pos_raw[i] = pos;
            vel_raw[i] = (int16_t)((speed & 0x8000) ? -(speed & 0x7FFF) : speed);
            rx_mask |= 1u << i;
        }
        if (rx_mask == all_mask) rx_done = 1;
        break;
    }
    }
}

static void drain_rx(void)
{
    bus_flush_input();
    rx_state = rx_done = 0;
    rx_mask = 0;
}

static void wait_rx(double deadline)
{
    uint8_t buf[256];
    while (!rx_done) {
        const double left = deadline - os_now();
        if (left <= 0) break;
        const int n = bus_read(buf, sizeof buf, left);
        for (int i = 0; i < n; i++) parse_byte(buf[i]);
    }
}

/* Reads len registers of the IMU board from addr into imu_rx; 1 when it replied. */
static int imu_read(uint8_t addr, int len)
{
    const uint8_t req[2] = {addr, (uint8_t)len};
    drain_rx();
    imu_want = len;
    imu_seen = 0;
    scs_write(IMU_ID, 0x02, req, 2);
    wait_rx(os_now() + REPLY_WAIT);
    imu_want = -1;
    return imu_seen;
}

static int16_t le16(const uint8_t *p)
{
    return (int16_t)(p[0] | p[1] << 8);
}

void bus_task(void *arg)
{
    (void)arg;
    float grav[3] = {0, 0, -1}, gyro_f[3] = {0}, vel_f[MAX_MOTORS] = {0};
    float lv[2], lsum[3], llo[3], lhi[3];
    int level_n = -1;
    double last_t = 0, last_imu = 0, last_healthy = 0, last_slow = 0, hz_t0 = os_now();
    uint16_t imu_count = 0;
    int enabled = 0, prev_enabled = 0, timed_out = 0, bus_mode = MODE_PREVIEW, bus_cal_hold = 0, fusion_ok = 0, ticks = 0;
    int imu_status = 0;
    uint32_t overruns = 0, imu_misses = 0;
    uint8_t req[2 + MAX_MOTORS] = {ADDR_PRESENT, PRESENT_LEN};
    for (int i = 0; i < C.n; i++) req[2 + i] = C.id[i];
    all_mask = (1u << C.n) - 1;

    os_lock(&d.bus_lock);
    for (int i = 0; i < 3; i++) {
        set_torque(0);
        os_sleep(0.005);
    }
    set_run_kp_kd();
    os_lock(&d.lock);
    memcpy(pos_raw, d.zero_pos, sizeof pos_raw);
    os_unlock(&d.lock);
    os_unlock(&d.bus_lock);

    double deadline = os_now();
    for (;;) {
        const double start = os_now();
        uint16_t des[MAX_MOTORS];

        os_lock(&d.bus_lock);
        os_lock(&d.lock);
        const int mode = d.mode;
        const int want = mode == MODE_HOLD || mode == MODE_WALK;
        const int arm = d.arm_request;
        const double target_time = d.target_time;
        const int cal_cmd = d.imu_cal_cmd;
        const int level_cmd = d.level_cmd;
        const int cal_hold = d.cal_hold;
        d.arm_request = 0;
        d.imu_cal_cmd = 0;
        d.level_cmd = 0;
        memcpy(lv, d.imu_level, sizeof lv);
        for (int i = 0; i < C.n; i++) {
            des[i] = mode == MODE_CALIBRATE ? d.cal_target[i]
                   : sat_pos(d.zero_pos[i] + (int)(d.target[i] / 360.0f * 4095.0f * C.motor_sign));
        }
        os_unlock(&d.lock);

        if (mode != bus_mode || (mode == MODE_CALIBRATE && cal_hold != bus_cal_hold)) {
            if (mode == MODE_CALIBRATE && cal_hold) {
                set_temp_kp_kd(C.cal_kp, 0);
                set_torque(1);
            } else if (mode == MODE_CALIBRATE || bus_mode == MODE_CALIBRATE || bus_mode == MODE_SERVO || mode == MODE_SERVO) {
                set_torque(0);
                set_run_kp_kd();
            }
            bus_mode = mode;
            bus_cal_hold = cal_hold;
        }
        const int bus_taken = mode == MODE_CALIBRATE || mode == MODE_SERVO;
        if (bus_taken || !want) enabled = timed_out = 0;
        if (arm && want && !enabled && imu_ok && start - last_imu < 0.05) {
            enabled = 1;
            timed_out = 0;
            last_healthy = start;
            set_run_kp_kd();
        }
        /* Stop on a policy loop silent for 250 ms or an IMU silent for 100 ms. */
        if (enabled && (start - target_time > 0.25 || start - last_healthy > 0.1)) {
            enabled = 0;
            timed_out = 1;
        }
        /* Writing a goal drags a servo even with torque off, so only write while enabled. */
        if ((mode == MODE_CALIBRATE && cal_hold) || enabled) sync_write_goals(des);
        if (!bus_taken) {
            if (enabled != prev_enabled || !enabled) set_torque(enabled);
            prev_enabled = enabled;
        } else {
            prev_enabled = 0;
        }

        drain_rx();
        scs_write(0xFE, 0x82, req, 2 + C.n);
        wait_rx(os_now() + REPLY_WAIT);
        const uint32_t mask = rx_mask;
        uint16_t pos[MAX_MOTORS];
        float vel[MAX_MOTORS];
        memcpy(pos, pos_raw, sizeof pos);
        for (int i = 0; i < C.n; i++) vel[i] = vel_raw[i] * 50.0f * (360.0f / 4096.0f) * C.motor_sign;

        /* The IMU board answers after the servos; a stale count or a cleared valid bit is no data. */
        imu_ok = 0;
        if (imu_read(IMU_FAST_ADDR, IMU_FAST_LEN)) {
            const uint16_t count = (uint16_t)(imu_rx[12] | imu_rx[13] << 8);
            for (int k = 0; k < 3; k++) {
                const int a = C.imu_map[k];
                acc[k] = C.imu_sign[k] * le16(imu_rx + 2 * a) * 9.8f / 1000;
                gyro[k] = C.imu_sign[k] * le16(imu_rx + 6 + 2 * a) / 10.0f;
            }
            imu_status = imu_rx[14];
            imu_ok = (imu_status & 1) && count != imu_count;
            imu_count = count;
        }
        if (imu_ok) last_imu = last_healthy = os_now();
        else imu_misses++;
        if (start - last_slow >= 1.0) {
            last_slow = start;
            if (imu_read(IMU_SLOW_ADDR, IMU_SLOW_LEN)) {
                os_lock(&d.lock);
                for (int k = 0; k < 3; k++) d.imu_bias[k] = le16(imu_rx + 2 * k) / 100.0f;
                d.battery_mv = imu_rx[6] | imu_rx[7] << 8;
                d.imu_cal_result = imu_rx[11];
                for (int k = 0; k < 2; k++) d.imu_mount[k] = le16(imu_rx + 12 + 2 * k) / 100.0f;
                os_unlock(&d.lock);
            }
        }
        /* Last on the bus, so the board's write reply cannot collide with the next request. */
        if (cal_cmd) {
            write_byte(IMU_ID, IMU_CAL_CMD, (uint8_t)cal_cmd);
            os_lock(&d.lock);
            d.imu_cal_result = 1;
            os_unlock(&d.lock);
        }
        os_unlock(&d.bus_lock);

        /* Leveling: 2 s of the unleveled accel, still within +-0.2 m/s^2 (the servos hum in the default pose). */
        if (level_cmd) level_n = 0;
        if (imu_ok && level_n >= 0) {
            for (int k = 0; k < 3; k++) {
                lsum[k] = level_n ? lsum[k] + acc[k] : acc[k];
                llo[k] = level_n ? fminf(llo[k], acc[k]) : acc[k];
                lhi[k] = level_n ? fmaxf(lhi[k], acc[k]) : acc[k];
            }
            if (++level_n == LEVEL_SAMPLES) {
                level_n = -1;
                const float m[3] = {lsum[0] / LEVEL_SAMPLES, lsum[1] / LEVEL_SAMPLES, lsum[2] / LEVEL_SAMPLES};
                /* Roll levels y, then pitch levels x, as level() applies them. */
                const float roll = atan2f(-m[1], -m[2]);
                const float pitch = atan2f(m[0], -(sinf(roll) * m[1] + cosf(roll) * m[2]));
                int ok = fabsf(roll) < 15 * (float)M_PI / 180 && fabsf(pitch) < 15 * (float)M_PI / 180;
                for (int k = 0; k < 3; k++) ok = ok && lhi[k] - llo[k] <= 0.4f;
                /* Kept by app/calib.c, which sees level_result turn 2: no files on this thread. */
                if (ok) {
                    lv[0] = roll * 180 / (float)M_PI;
                    lv[1] = pitch * 180 / (float)M_PI;
                }
                os_lock(&d.lock);
                if (ok) memcpy(d.imu_level, lv, sizeof lv);
                d.level_result = ok ? 2 : 3;
                os_unlock(&d.lock);
            }
        }
        if (imu_ok) {
            level(acc, lv);
            level(gyro, lv);
        }

        /*
         * IMU filter: gyro prediction plus accel-magnitude-weighted correction of
         * body-frame gravity, and smoothing of gyro and joint velocity. A missed
         * sample (the bus is busy in a roll) keeps the estimate; only a gap over
         * 100 ms restarts it from the accelerometer.
         */
        const double t = os_now();
        if (!imu_ok) {
            if (last_t != 0 && t - last_t > 0.1) {
                last_t = 0;
                fusion_ok = 0;
            }
        } else {
            const float dt_raw = last_t == 0 ? (float)TICK : (float)(t - last_t);
            const float norm = sqrtf(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
            const float dt = fminf(fmaxf(dt_raw, 0.001f), 0.1f);
            if (last_t == 0 || dt_raw > 0.1f) {
                for (int k = 0; k < 3; k++) {
                    grav[k] = norm > 1e-6f ? acc[k] / norm : (k == 2 ? -1.0f : 0.0f);
                    gyro_f[k] = gyro[k];
                }
                memcpy(vel_f, vel, sizeof vel_f);
            } else {
                const float wx = gyro[0] * (float)M_PI / 180, wy = gyro[1] * (float)M_PI / 180;
                const float wz = gyro[2] * (float)M_PI / 180;
                /* A world-fixed vector in rotating body coordinates: dg/dt = -omega x g. */
                float p[3] = {grav[0] + (wz * grav[1] - wy * grav[2]) * dt,
                              grav[1] + (wx * grav[2] - wz * grav[0]) * dt,
                              grav[2] + (wy * grav[0] - wx * grav[1]) * dt};
                float pn = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                const float trust = fmaxf(0.0f, 1.0f - fabsf(norm / 9.8f - 1.0f) / 0.2f);
                const float gain = norm > 1e-6f ? (1.0f - expf(-dt / 0.3f)) * trust : 0.0f;
                for (int k = 0; k < 3; k++) p[k] = (1 - gain) * p[k] / pn + (norm > 1e-6f ? gain * acc[k] / norm : 0);
                pn = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                /* Keep the 100 Hz smoothing constants when samples arrive late. */
                const float ga = powf(0.5f, dt / (float)TICK), va = powf(0.4f, dt / (float)TICK);
                for (int k = 0; k < 3; k++) {
                    grav[k] = p[k] / pn;
                    gyro_f[k] = ga * gyro_f[k] + (1 - ga) * gyro[k];
                }
                for (int i = 0; i < C.n; i++) vel_f[i] = va * vel_f[i] + (1 - va) * vel[i];
            }
            last_t = t;
            fusion_ok = 1;
        }

        ticks++;
        os_lock(&d.lock);
        d.have_fb = 1;
        d.rx_time = t;
        d.imu_ok = imu_ok;
        d.imu_time = last_imu;
        d.imu_misses = imu_misses;
        d.imu_status = imu_status;
        d.enabled = enabled;
        d.timed_out = timed_out;
        d.bus_mode = bus_mode;
        d.servo_mask = mask;
        memcpy(d.acc, acc, sizeof acc);
        memcpy(d.gyro, gyro, sizeof gyro);
        for (int i = 0; i < C.n; i++) {
            d.raw_pos[i] = pos[i];
            d.angle[i] = (float)(pos[i] - d.zero_pos[i]) * 360.0f / 4095.0f * C.motor_sign;
        }
        d.fusion_ok = fusion_ok;
        memcpy(d.grav, grav, sizeof grav);
        for (int k = 0; k < 3; k++) d.gyro_rad[k] = gyro_f[k] * (float)M_PI / 180;
        memcpy(d.vel, vel_f, sizeof vel_f);
        if (t - hz_t0 >= 1.0) {
            d.feedback_hz = (float)(ticks / (t - hz_t0));
            ticks = 0;
            hz_t0 = t;
        }
        d.bus_work_us = (uint32_t)((os_now() - start) * 1e6);
        d.bus_overruns = overruns;
        os_unlock(&d.lock);

        deadline += TICK;
        if (deadline <= os_now()) {
            overruns++;
            deadline = os_now();
        }
        os_sleep_until(deadline);
    }
}

int servo_op(int op, int id, int a, int b, int *raw_out)
{
    int ok = 1;
    os_lock(&d.bus_lock);
    switch (op) {
    case SERVO_TORQUE_OFF:
        set_torque(0);
        set_run_kp_kd();
        break;
    case SERVO_UNLOCK:
        write_byte((uint8_t)id, ADDR_UNLOCK, 0);
        break;
    case SERVO_SET_ID:
        set_torque(0);
        write_byte((uint8_t)id, ADDR_UNLOCK, 0);
        os_sleep(0.003);
        write_byte((uint8_t)id, ADDR_ID, (uint8_t)a);
        break;
    case SERVO_GOTO: {
        const uint8_t data[7] = {ADDR_GOAL_POS, (uint8_t)(a & 0xFF), (uint8_t)(a >> 8)};
        set_run_kp_kd();
        set_torque(1);
        scs_write((uint8_t)id, 0x03, data, 7);
        break;
    }
    case SERVO_SET_GAINS: {
        const uint8_t data[3] = {ADDR_PERM_KP_KD, (uint8_t)a, (uint8_t)b};
        write_byte((uint8_t)id, ADDR_UNLOCK, 0);
        os_sleep(0.003);
        scs_write((uint8_t)id, 0x03, data, 3);
        break;
    }
    case SERVO_PING:
        drain_rx();
        ping_id = id;
        ping_seen = 0;
        scs_write((uint8_t)id, 0x01, NULL, 0);
        wait_rx(os_now() + REPLY_WAIT);
        ok = ping_seen;
        ping_id = -1;
        break;
    case SERVO_READ: {
        const uint8_t data[2] = {ADDR_PRESENT, PRESENT_LEN};
        drain_rx();
        read_id = id;
        read_seen = read_pos = 0;
        scs_write((uint8_t)id, 0x02, data, 2);
        wait_rx(os_now() + REPLY_WAIT);
        ok = read_seen;
        if (raw_out) *raw_out = read_pos;
        read_id = -1;
        break;
    }
    }
    os_unlock(&d.bus_lock);
    return ok;
}
