#ifndef DUCK_H
#define DUCK_H

/*
 * The control core: the robot description (C, filled from robots/<name>.json
 * before anything starts) and the state shared by the bus loop, the policy
 * loop and the services (d, guarded by d.lock). Platform independent: it uses
 * osal.h, bus.h and storage.h only, no allocation and no files.
 */
#include <stdint.h>

#include "osal.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_MOTORS 16
#define MAX_POLICIES 16
#define OBS_DIM 61
#define ACT_DIM 14

enum { MODE_PREVIEW, MODE_OFF, MODE_HOLD, MODE_WALK, MODE_CALIBRATE, MODE_SERVO, MODE_COUNT };
enum { PHASE_WALK, PHASE_DEFAULT_POSE, PHASE_GETUP };
/* Control sources, highest priority first. */
enum { SRC_NONE, SRC_PAD, SRC_WEB, SRC_VOICE, SRC_LLM, SRC_GESTURE, SRC_COUNT };
enum { POLICY_GAIT, POLICY_RECOVERY, POLICY_SKILL };
/*
 * What a skill feeds the policy as its command: a fixed zero twist, a phase
 * cos/sin over its period, or a posture flag in vx (1 sit, 0 stand).
 */
enum { CMD_CONSTANT, CMD_PHASE, CMD_POSTURE };
/* Servo setup operations; the numbers are the op codes servo.op accepts. */
enum { SERVO_TORQUE_OFF = 0, SERVO_UNLOCK = 2, SERVO_SET_ID, SERVO_GOTO, SERVO_SET_GAINS, SERVO_READ, SERVO_PING };

/* One entry of the policy table, from its manifest (app/policies.c). An empty name is a free entry. */
struct policy_cfg {
    char name[24];      /* what clients ask for */
    int kind, command;
    float period_s;     /* a skill's phase goes 0 -> 1 over period_s and the skill ends at end_phase */
    float end_phase;
    float ramp_s;       /* a posture skill ends this long after it was told to stand */
    float alpha;        /* action smoothing while it runs; < 0 keeps the user setting */
    float mouth_deg;    /* mouth while phase < mouth_until; 0 leaves the mouth alone */
    float mouth_until;
    int hold_recovery;  /* tilt does not select get-up while it runs */
    int verified;       /* imported ones run from the web page only until verified */
    float home[MAX_MOTORS]; /* deg: the HOME pose it was trained around ("home_deg"), else the robot's */
    const void *mlp;    /* loaded weights */
};

struct robot_cfg {
    char name[32];
    int n;                              /* motors */
    uint8_t id[MAX_MOTORS];
    float home[MAX_MOTORS];             /* deg */
    uint16_t factory_zero[MAX_MOTORS];
    float motor_sign;                   /* encoder direction against the joint angle */
    int mouth;                          /* motor index, or -1 */
    float mouth_max;
    int act_motor[ACT_DIM];             /* motor index of each policy action */
    int head_act_begin, head_act_end;   /* actions smoothed with the head alpha */
    uint8_t kp, kd, mouth_kp, cal_kp;
    uint8_t motor_kp[MAX_MOTORS];       /* a motor's own P ("kp_by_id"), 0: kp */
    int imu_map[3];                     /* body axis k = imu_sign[k] * IMU board axis imu_map[k] */
    float imu_sign[3];
    float vmax[SRC_COUNT][3];           /* per source |vx|, |vy|, |wz| */
    float fall_deg, fall_s, stand_deg, stand_s, hold_s;
    int low_battery_mv;
    /* The policy table, changed at run time by imports, under d.lock: entries in use, the walk and get-up ones or -1. */
    int np, gait, recovery;
    struct policy_cfg p[MAX_POLICIES];
};

struct duck {
    os_mutex_t lock;
    /* Serializes the bus: the bus loop and servo setup operations. */
    os_mutex_t bus_lock;

    /* Written by the bus loop every 10 ms. */
    int have_fb;
    double rx_time;
    int imu_ok, enabled, timed_out;
    double imu_time;                    /* last good IMU sample */
    uint32_t imu_misses;                /* IMU reads that failed or were stale */
    int bus_mode;                       /* the mode the bus loop has applied to the servos */
    uint32_t servo_mask;
    float acc[3], gyro[3];              /* m/s^2, deg/s, body axes */
    float angle[MAX_MOTORS];            /* deg */
    uint16_t raw_pos[MAX_MOTORS];
    int fusion_ok;
    float grav[3], gyro_rad[3], vel[MAX_MOTORS];
    float feedback_hz;
    uint32_t bus_work_us, bus_overruns;
    /* IMU board, read once per second. */
    int imu_status, imu_cal_result, battery_mv;
    float imu_bias[3], imu_mount[2];
    /* Torso leveling done on this board (imu_level.json): roll, pitch in deg; result 0 idle, 1 measuring, 2 done, 3 failed. */
    float imu_level[2];
    int level_cmd, level_result;

    /* Written by the policy loop and the services. */
    int mode, generation;
    uint16_t zero_pos[MAX_MOTORS];
    int cal_hold;                       /* calibration: the servos hold cal_target with a weak force; else torque is off */
    uint16_t cal_target[MAX_MOTORS];
    int cal_done[MAX_MOTORS];
    float target[MAX_MOTORS];           /* deg, the latest command for the servos */
    double target_time;
    int arm_request, arm_pending;
    int imu_cal_cmd;
    float twist[3], head[4], mouth;
    float alpha_leg, alpha_head;
    /* Arbitration: who holds control and until when. */
    int owner;
    double lease_until, move_until;
    /* Fall recovery and skills. */
    int phase, needs_reset;
    float tilt_deg;
    int tilt_valid;
    float fall_elapsed, stand_elapsed, hold_elapsed;
    double phase_since;
    int skill;                          /* policy index, or -1 */
    float skill_phi;                    /* 0..1 through the skill */
    double stand_at;                    /* a posture skill was told to stand, or 0 while it sits */
    int infer_policy;                   /* the policy whose weights the policy loop is using, or -1 */
    float inference_hz, inference_ms;
    const char *last_error;             /* error code (app/api.c ERRORS), or NULL */
};

extern struct robot_cfg C;
extern struct duck d;
extern const char *const MODE_NAMES[MODE_COUNT];
extern const char *const SOURCE_NAMES[SRC_COUNT];

/* bus.c */
void bus_task(void *arg);
/* Runs one servo setup operation; returns 1 when it succeeded or the servo replied. */
int servo_op(int op, int id, int a, int b, int *raw_out);

/* policy.c */
unsigned long policy_size(void);        /* bytes of one weight file */
void policy_infer(const void *weights, const float *obs, float *action);

/* motion.c */
void duck_init(void);                   /* start-up state, once C is filled */
void motion_task(void *arg);
void reset_motion(void);                /* with d.lock held */

/*
 * arbiter.c: every client request goes through these. Call with d.lock held;
 * each returns NULL or an error code.
 */
const char *arb_claim(int src, double now);
const char *arb_set_mode(int src, int mode, double now);
const char *arb_move(int src, const float twist[3], double duration, double now);
const char *arb_head(int src, const float head[4], double now);
const char *arb_mouth(int src, float deg, double now);
const char *arb_skill(int src, const char *name, double now);
void arb_stop(int src, double now);     /* zero velocity, from anyone */
void arb_estop(int src, double now);    /* torque off, from anyone */
void arb_tick(double now);              /* lease expiry, from the policy loop */

#endif
