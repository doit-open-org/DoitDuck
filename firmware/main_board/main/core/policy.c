/*
 * The policy network: (obs - mean) / std -> 512 -> 256 -> 128 -> 14 with ELU,
 * float32 weights in the layout tools/import_policy.py writes.
 */
#include "duck.h"

#include <math.h>

struct mlp {
    float mean[OBS_DIM], std[OBS_DIM];
    float w0[512][OBS_DIM], b0[512];
    float w1[256][512], b1[256];
    float w2[128][256], b2[128];
    float w3[ACT_DIM][128], b3[ACT_DIM];
};

unsigned long policy_size(void)
{
    return sizeof(struct mlp);
}

static void layer(const float *w, const float *b, const float *in, float *out, int n_in, int n_out, int elu)
{
    for (int o = 0; o < n_out; o++) {
        const float *row = w + o * n_in;
        float s = b[o];
        for (int i = 0; i < n_in; i++) s += row[i] * in[i];
        out[o] = elu && s < 0 ? expf(s) - 1.0f : s;
    }
}

void policy_infer(const void *weights, const float *obs, float *action)
{
    const struct mlp *m = weights;
    float x[OBS_DIM], h0[512], h1[256], h2[128];
    for (int i = 0; i < OBS_DIM; i++) x[i] = (obs[i] - m->mean[i]) / m->std[i];
    layer(&m->w0[0][0], m->b0, x, h0, OBS_DIM, 512, 1);
    layer(&m->w1[0][0], m->b1, h0, h1, 512, 256, 1);
    layer(&m->w2[0][0], m->b2, h1, h2, 256, 128, 1);
    layer(&m->w3[0][0], m->b3, h2, action, 128, ACT_DIM, 0);
}
