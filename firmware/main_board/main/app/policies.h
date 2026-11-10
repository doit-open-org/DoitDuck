#ifndef POLICIES_H
#define POLICIES_H

/*
 * The policy library (policies.c): fills the core's policy table C.p from the
 * robot's built-in policies and the user's imported ones, and changes it at
 * run time through the policy.* methods.
 */
#include "cJSON.h"
#include "feature.h"

void policies_load(void);
/* The skills, {name, label, verified, posture}, into array a, for robot.info. */
void policies_skills(cJSON *a);

extern const struct api_method POLICY_METHODS[];
extern const int NPOLICY_METHODS;

#endif
