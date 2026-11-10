#ifndef CALIB_H
#define CALIB_H

/* Calibration (calib.c): zero positions, IMU level, the wizard's steps, data/calibration.json. */
#include "feature.h"

void calib_load(void);    /* at start, once C is filled */
void calib_watch(void);   /* from app.c's loop: keeps a finished IMU leveling */

extern const struct api_method CALIB_METHODS[];
extern const int NCALIB_METHODS;

#endif
