/*
 * Runs the robot's policy code (firmware/main_board/main/core/policy.c) on a file of observations, to
 * compare with another build or with onnxruntime: obs.f32 holds N x 61 float32,
 * actions.f32 gets N x 14 float32.
 *
 *   M=firmware/main_board
 *   cc -O2 -I$M/main/core -I$M/components/osal -I$M/components/osal/linux tools/policy_check.c \
 *       $M/main/core/policy.c -lm -o policy_check
 *   ./policy_check weights.bin obs.f32 actions.f32
 */
#include <stdio.h>
#include <stdlib.h>

#include "duck.h"

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s weights.bin obs.f32 actions.f32\n", argv[0]);
        return 2;
    }
    FILE *w = fopen(argv[1], "rb"), *in = fopen(argv[2], "rb"), *out = fopen(argv[3], "wb");
    void *weights = malloc(policy_size());
    if (!w || !in || !out || !weights || fread(weights, 1, policy_size(), w) != policy_size() || fgetc(w) != EOF) {
        fprintf(stderr, "cannot read %s as %lu bytes of weights, or open the other files\n", argv[1], policy_size());
        return 1;
    }
    float obs[OBS_DIM], action[ACT_DIM];
    int n = 0;
    while (fread(obs, sizeof obs, 1, in) == 1) {
        policy_infer(weights, obs, action);
        fwrite(action, sizeof action, 1, out);
        n++;
    }
    fclose(out);
    printf("%d observations\n", n);
    return 0;
}
