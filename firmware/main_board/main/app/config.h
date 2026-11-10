#ifndef CONFIG_H
#define CONFIG_H

/*
 * robots/<name>.json fills the core's robot description C; the board settings
 * B (how this board reaches the bus and serves the web page) come from Kconfig.
 * Zero positions and the IMU level are calib.c's.
 */
#include "audio.h"
#include "bus.h"
#include "cJSON.h"

struct board_cfg {
    char name[32];
    char robot[32];          /* robots/<robot>.json */
    struct bus_cfg bus;
    int http_port;
    struct audio_cfg audio;
};

extern struct board_cfg B;

void config_load_board(void);
int config_load_robot(const char *name);   /* 0 on success; prints why not */
/* A top-level section of the robot file, e.g. "voice"; NULL when absent. */
const cJSON *config_section(const char *key);
const cJSON *config_robot(void);

#endif
