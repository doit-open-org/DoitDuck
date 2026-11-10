/*
 * 50 Hz policy loop: picks the policy (gait, get-up or a skill), builds the
 * observation, runs it and hands the targets to the bus loop. The fall / get-up
 * state machine and the skill timing live here; which policies exist and how a
 * skill is driven come from the robot config.
 */
#include "duck.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PERIOD 0.02

struct robot_cfg C;
struct duck d = {.lock = OS_MUTEX_INIT, .bus_lock = OS_MUTEX_INIT};

/* Only the selected policy runs and a switch always resets, so one history serves all of them. */
static float prev_action[ACT_DIM], act_filt[ACT_DIM];

void duck_init(void)
{
    /* Boot in preview: the policy runs, torque stays off. */
    d.mode = MODE_PREVIEW;
    d.phase_since = -1;
    d.skill = -1;
    d.infer_policy = -1;
    memcpy(d.target, C.home, sizeof d.target);
    for (int i = 0; i < C.n; i++) d.cal_target[i] = 2047;
}

/* Mode-switch side effects; call with d.lock held. */
void reset_motion(void)
{
    d.generation++;
    d.phase = PHASE_WALK;
    d.tilt_valid = 0;
    d.phase_since = -1;
    d.fall_elapsed = d.stand_elapsed = d.hold_elapsed = 0;
    d.needs_reset = 1;
    d.skill = -1;
    d.skill_phi = 0;
    d.stand_at = 0;
}

/* Fall / get-up state machine on the 50 Hz clock; returns 1 when the phase changed. */
static int recovery_update(const float *g, double now, int valid)
{
    const float norm = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    if (!valid || !isfinite(norm) || norm < 1e-6f) {
        d.fall_elapsed = d.stand_elapsed = 0;
        d.tilt_valid = 0;
        if (d.phase == PHASE_DEFAULT_POSE) {
            d.phase_since = -1;
            d.hold_elapsed = 0;
        }
        return 0;
    }
    d.tilt_deg = acosf(fmaxf(-1.0f, fminf(1.0f, -g[2] / norm))) * 180.0f / (float)M_PI;
    d.tilt_valid = 1;
    const int before = d.phase;
    if (d.phase == PHASE_WALK) {
        if (d.tilt_deg > C.fall_deg) {
            d.fall_elapsed += (float)PERIOD;
            if (d.fall_elapsed >= C.fall_s) {
                d.phase = PHASE_DEFAULT_POSE;
                d.phase_since = now;
                d.hold_elapsed = 0;
            }
        } else {
            d.fall_elapsed = 0;
        }
    } else if (d.phase == PHASE_DEFAULT_POSE) {
        if (d.phase_since < 0) d.phase_since = now;
        d.hold_elapsed = (float)(now - d.phase_since);
        if (now >= d.phase_since + C.hold_s) {
            d.phase = PHASE_GETUP;
            d.stand_elapsed = 0;
        }
    } else {
        if (d.tilt_deg < C.stand_deg) {
            d.stand_elapsed += (float)PERIOD;
            if (d.stand_elapsed >= C.stand_s) {
                d.phase = PHASE_WALK;
                d.fall_elapsed = 0;
            }
        } else {
            d.stand_elapsed = 0;
        }
    }
    return before != d.phase;
}

static void end_skill(void)
{
    d.skill = -1;
    d.skill_phi = 0;
    d.stand_at = 0;
    d.needs_reset = 1;
}

static void step(void)
{
    float obs[OBS_DIM] = {0}, target[MAX_MOTORS], twist[3], head[4], grav[3], gyro[3];
    float angle[MAX_MOTORS], vel[MAX_MOTORS];
    const double now = os_now();
    static int inferred;
    static double hz_t0;

    os_lock(&d.lock);
    if (now - hz_t0 >= 1.0) {
        d.inference_hz = (float)(inferred / (now - hz_t0));
        inferred = 0;
        hz_t0 = now;
    }
    arb_tick(now);
    const int fresh = d.have_fb && now - d.rx_time < 0.15;
    /* Same 100 ms as the bus watchdog: a single missed IMU sample is not a lost IMU. */
    const int imu_alive = now - d.imu_time < 0.1;
    if ((d.mode == MODE_HOLD || d.mode == MODE_WALK) && (!fresh || !imu_alive)) {
        d.mode = MODE_PREVIEW;
        d.arm_pending = 0;
        reset_motion();
        d.last_error = "feedback_lost";
    }
    const int mode = d.mode, generation = d.generation;
    memcpy(twist, d.twist, sizeof twist);
    memcpy(head, d.head, sizeof head);
    float mouth = d.mouth;
    const int valid = fresh && imu_alive && d.fusion_ok;
    const struct policy_cfg *sk = d.skill >= 0 ? &C.p[d.skill] : NULL;
    if (sk && d.phase != PHASE_WALK) {
        end_skill();
        sk = NULL;
    }
    if ((mode == MODE_PREVIEW || mode == MODE_WALK) && !(sk && sk->hold_recovery)) {
        const int changed = recovery_update(d.grav, now, valid);
        d.needs_reset |= changed;
        if (changed && d.phase != PHASE_WALK && sk) {
            end_skill();
            sk = NULL;
        }
    }
    int policy = d.phase == PHASE_WALK ? C.gait : C.recovery;
    float alpha_leg = d.alpha_leg, alpha_head = d.alpha_head;
    if (sk) {
        policy = d.skill;
        memset(head, 0, sizeof head);
        memset(twist, 0, sizeof twist);
        if (sk->command == CMD_PHASE) {
            const float a = 2.0f * (float)M_PI * d.skill_phi;
            twist[0] = cosf(a);
            twist[1] = sinf(a);
        } else if (sk->command == CMD_POSTURE) {
            twist[0] = d.stand_at > 0 ? 0.0f : 1.0f;
        }
        if (sk->mouth_deg > 0) mouth = d.mouth = d.skill_phi < sk->mouth_until ? sk->mouth_deg : 0.0f;
    }
    if (policy >= 0 && C.p[policy].alpha >= 0) alpha_leg = alpha_head = C.p[policy].alpha;
    const int phase = d.phase, reset = d.needs_reset;
    /* With no walk or get-up policy imported, nothing runs: the duck can only preview or hold the default pose. */
    const int inferring = (mode == MODE_PREVIEW || mode == MODE_WALK) && valid && policy >= 0;
    if (inferring) d.needs_reset = 0;
    /* app/policies.c leaves these weights alone until the loop lets go of them below. */
    if (inferring && phase != PHASE_DEFAULT_POSE) d.infer_policy = policy;
    memcpy(grav, d.grav, sizeof grav);
    memcpy(gyro, d.gyro_rad, sizeof gyro);
    memcpy(angle, d.angle, sizeof angle);
    memcpy(vel, d.vel, sizeof vel);
    os_unlock(&d.lock);
    if (mode == MODE_CALIBRATE || mode == MODE_SERVO) return;

    memcpy(target, C.home, sizeof target);
    if (inferring && (phase == PHASE_DEFAULT_POSE || reset)) {
        memset(prev_action, 0, sizeof prev_action);
        memset(act_filt, 0, sizeof act_filt);
    }
    /* In the fallen hold the action is forced to 0, which is the home pose. */
    if (inferring && phase != PHASE_DEFAULT_POSE) {
        float action[ACT_DIM];
        if (phase == PHASE_GETUP) {
            memset(twist, 0, sizeof twist);
            memset(head, 0, sizeof head);
        }
        /* gyro, gravity, joint position relative to home, joint velocity, previous action, twist, head, six zeros. */
        memcpy(obs, gyro, sizeof gyro);
        memcpy(obs + 3, grav, sizeof grav);
        for (int i = 0; i < ACT_DIM; i++) {
            const int j = C.act_motor[i];
            obs[6 + i] = (angle[j] - C.p[policy].home[j]) * (float)M_PI / 180;
            obs[20 + i] = vel[j] * (float)M_PI / 180;
        }
        memcpy(obs + 34, prev_action, sizeof prev_action);
        memcpy(obs + 48, twist, sizeof twist);
        memcpy(obs + 51, head, sizeof head);

        const double begin = os_now();
        policy_infer(C.p[policy].mlp, obs, action);
        const double finish = os_now();

        int finite = 1;
        for (int i = 0; i < ACT_DIM; i++) finite = finite && isfinite(action[i]);
        if (!finite) {
            os_lock(&d.lock);
            d.mode = MODE_OFF;
            d.arm_pending = 0;
            d.infer_policy = -1;
            reset_motion();
            d.last_error = "policy_not_finite";
            os_unlock(&d.lock);
            return;
        }
        memcpy(prev_action, action, sizeof action);
        for (int i = 0; i < ACT_DIM; i++) {
            const float a = i >= C.head_act_begin && i < C.head_act_end ? alpha_head : alpha_leg;
            const int j = C.act_motor[i];
            act_filt[i] = act_filt[i] * a + action[i] * (1 - a);
            target[j] = C.p[policy].home[j] + act_filt[i] * 180.0f / (float)M_PI;
        }

        os_lock(&d.lock);
        d.infer_policy = -1;
        d.inference_ms = (float)((finish - begin) * 1000);
        inferred++;
        if (sk && d.skill == policy) {
            const int done = sk->command == CMD_POSTURE ? d.stand_at > 0 && os_now() - d.stand_at >= sk->ramp_s
                                                        : (d.skill_phi += (float)(PERIOD / sk->period_s)) >= sk->end_phase;
            if (done) {
                end_skill();
                d.fall_elapsed = 0;
            }
        }
        os_unlock(&d.lock);
    }

    os_lock(&d.lock);
    /* A mode change during inference always wins; never send its obsolete action. */
    if (d.generation == generation && d.mode != MODE_CALIBRATE && d.mode != MODE_SERVO) {
        if (d.arm_pending) {
            /* The first target after torque on is the measured pose. */
            memcpy(target, d.angle, sizeof target);
            d.arm_request = 1;
            d.arm_pending = 0;
        } else if (C.mouth >= 0) {
            target[C.mouth] = mouth;
        }
        memcpy(d.target, target, sizeof target);
        d.target_time = os_now();
    }
    os_unlock(&d.lock);
}

void motion_task(void *arg)
{
    (void)arg;
    double deadline = os_now();
    for (;;) {
        step();
        deadline += PERIOD;
        /* A late cycle is skipped, never queued. */
        if (deadline <= os_now()) deadline = os_now();
        os_sleep_until(deadline);
    }
}
