/*
 * Who controls the robot. One source holds control at a time and keeps it with
 * a lease: the gamepad and the web page renew it with every command; voice and
 * the LLM send one-shot commands whose movement carries its own end time. A
 * higher-priority source can always take over; stop and torque off work from
 * anyone. Every function expects d.lock held.
 */
#include "duck.h"

#include <math.h>
#include <string.h>

#define LEASE_S 1.0
#define SETUP_LEASE_S 2.0   /* calibration and servo setup */
#define MAX_MOVE_S 5.0

const char *const MODE_NAMES[MODE_COUNT] = {"preview", "off", "hold", "walk", "calibrate", "servo"};
const char *const SOURCE_NAMES[SRC_COUNT] = {"none", "pad", "web", "voice", "llm", "gesture"};
static const char *const BUSY[SRC_COUNT] = {"", "busy_pad", "busy_web", "busy_voice", "busy_llm", "busy_gesture"};

static int continuous(int src)
{
    return src == SRC_PAD || src == SRC_WEB;
}

static void renew(double now)
{
    const double lease = now + (d.mode >= MODE_CALIBRATE ? SETUP_LEASE_S : LEASE_S);
    d.lease_until = lease > d.move_until ? lease : d.move_until;
}

static const char *busy(int src, double now)
{
    return d.owner != SRC_NONE && d.owner != src && now < d.lease_until && src > d.owner ? BUSY[d.owner] : NULL;
}

static void take(int src, double now)
{
    if (d.owner != src) {
        /* A new holder starts from standing still, not from the old holder's command. */
        memset(d.twist, 0, sizeof d.twist);
        d.move_until = 0;
        d.owner = src;
    }
    renew(now);
}

const char *arb_claim(int src, double now)
{
    const char *err = busy(src, now);
    if (!err) take(src, now);
    return err;
}

const char *arb_set_mode(int src, int mode, double now)
{
    const int cur = d.mode;
    const int torque_new = mode == MODE_HOLD || mode == MODE_WALK;
    if (mode == MODE_OFF) {
        arb_estop(src, now);
        return NULL;
    }
    const char *err = busy(src, now);
    if (err) return err;
    if (mode == cur) {
        take(src, now);
        return NULL;
    }
    if (mode >= MODE_CALIBRATE && src != SRC_WEB) return "setup_web_only";
    if (torque_new && cur >= MODE_CALIBRATE) return "exit_setup_first";
    if (mode >= MODE_CALIBRATE && (cur == MODE_HOLD || cur == MODE_WALK)) return "torque_off_first";
    if (torque_new && (!d.have_fb || now - d.rx_time > 0.15)) return "no_feedback";
    if (torque_new && now - d.imu_time > 0.1) return "imu_offline";
    if (torque_new && d.battery_mv > 0 && d.battery_mv < C.low_battery_mv) return "battery_low";
    if (mode == MODE_WALK && (C.gait < 0 || C.recovery < 0)) return "no_base_policies";
    if (mode == MODE_WALK && src != SRC_WEB && !(C.p[C.gait].verified && C.p[C.recovery].verified)) return "policy_not_verified";
    take(src, now);
    d.mode = mode;
    reset_motion();
    d.last_error = NULL;
    memset(d.twist, 0, sizeof d.twist);
    memset(d.head, 0, sizeof d.head);
    d.mouth = 0;
    d.move_until = 0;
    d.arm_pending = torque_new && !d.enabled;
    if (cur == MODE_CALIBRATE || mode == MODE_CALIBRATE) memset(d.cal_done, 0, sizeof d.cal_done);
    if (mode == MODE_CALIBRATE) {
        for (int i = 0; i < C.n; i++) d.cal_target[i] = 2047;
    }
    renew(now);
    return NULL;
}

static float clampf(float v, float lim)
{
    return v > lim ? lim : v < -lim ? -lim : v;
}

const char *arb_move(int src, const float twist[3], double duration, double now)
{
    const char *err = busy(src, now);
    if (err) return err;
    if (d.mode >= MODE_CALIBRATE) return "in_setup";
    if (d.skill >= 0) return "skill_running";
    if (!continuous(src) && !(duration > 0)) return "duration_required";
    for (int k = 0; k < 3; k++) {
        if (!isfinite(twist[k])) return "bad_velocity";
    }
    take(src, now);
    for (int k = 0; k < 3; k++) d.twist[k] = clampf(twist[k], C.vmax[src][k]);
    d.move_until = duration > 0 ? now + (duration < MAX_MOVE_S ? duration : MAX_MOVE_S) : 0;
    renew(now);
    return NULL;
}

const char *arb_head(int src, const float head[4], double now)
{
    const char *err = busy(src, now);
    if (err) return err;
    if (d.mode >= MODE_CALIBRATE) return "in_setup";
    if (d.skill >= 0) return "skill_running";
    for (int k = 0; k < 4; k++) {
        if (!isfinite(head[k])) return "bad_head";
    }
    take(src, now);
    for (int k = 0; k < 4; k++) d.head[k] = clampf(head[k], 1.0f);
    return NULL;
}

const char *arb_mouth(int src, float deg, double now)
{
    const char *err = busy(src, now);
    if (err) return err;
    if (d.mode >= MODE_CALIBRATE) return "in_setup";
    if (d.skill >= 0) return "skill_running";
    if (!(deg >= 0 && deg <= C.mouth_max)) return "bad_mouth";
    take(src, now);
    d.mouth = deg;
    return NULL;
}

const char *arb_skill(int src, const char *name, double now)
{
    const char *err = busy(src, now);
    if (err) return err;
    int k = -1;
    for (int i = 0; i < C.np; i++) {
        if (C.p[i].kind == POLICY_SKILL && !strcmp(C.p[i].name, name)) k = i;
    }
    if (k < 0) return "no_such_skill";
    if (src != SRC_WEB && !C.p[k].verified) return "policy_not_verified";
    /* A posture skill sits until it is asked again, then stands. */
    if (d.skill == k && C.p[k].command == CMD_POSTURE) {
        take(src, now);
        if (d.stand_at == 0) d.stand_at = now;
        return NULL;
    }
    if (d.mode != MODE_PREVIEW && d.mode != MODE_WALK) return "skill_needs_walk";
    if (d.phase != PHASE_WALK || d.skill >= 0) return "skill_not_now";
    take(src, now);
    d.skill = k;
    d.skill_phi = 0;
    d.needs_reset = 1;
    d.last_error = NULL;
    memset(d.twist, 0, sizeof d.twist);
    d.move_until = 0;
    return NULL;
}

void arb_stop(int src, double now)
{
    (void)src;
    (void)now;
    memset(d.twist, 0, sizeof d.twist);
    d.move_until = 0;
}

void arb_estop(int src, double now)
{
    (void)src;
    (void)now;
    /* In mode off the bus loop disarms and writes torque 0 every tick. */
    if (d.mode != MODE_OFF) reset_motion();
    d.mode = MODE_OFF;
    d.arm_pending = 0;
    memset(d.twist, 0, sizeof d.twist);
    d.move_until = 0;
    d.owner = SRC_NONE;
}

void arb_tick(double now)
{
    if (d.move_until > 0 && now >= d.move_until) {
        memset(d.twist, 0, sizeof d.twist);
        d.move_until = 0;
    }
    if (d.owner == SRC_NONE || now < d.lease_until) return;
    /* The holder went quiet: stop moving and give control back. */
    if (continuous(d.owner)) memset(d.twist, 0, sizeof d.twist);
    if (d.mode == MODE_CALIBRATE || d.mode == MODE_SERVO) {
        d.mode = MODE_PREVIEW;
        reset_motion();
        memset(d.cal_done, 0, sizeof d.cal_done);
        d.last_error = "setup_heartbeat_lost";
    }
    d.owner = SRC_NONE;
}
