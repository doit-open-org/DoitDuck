#ifndef APP_H
#define APP_H

/*
 * Start-up shared by every platform: the board settings from Kconfig and
 * robots/<name>.json, starts the bus and policy loops, the microphone and the features, and
 * runs until stopped. A platform's main() only calls app_run.
 */
#include "cJSON.h"

int app_run(void);  /* exit status */
/* Stops app_run with the exit status that has the autostart script start the program again. */
void app_restart(void);
/* Adds the names of the running features to r[key], for robot.info. */
void app_features(cJSON *r, const char *key);
/* Adds each running feature's live state to r[feature name], for robot.status. */
void app_status(cJSON *r);

#endif
