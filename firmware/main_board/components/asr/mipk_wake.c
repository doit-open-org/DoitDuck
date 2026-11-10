/*
 * asr.h with keyword models on the Cove-610: mipk MicroWakeWord models
 * (mipk/mipk_model), one per word, each with the score threshold packed in it;
 * the thresholds of the config do not apply. The wake word's model is
 * models/<wake_model>.mipk ("你好小鸭" by default, nihaoxiaoya); a command word
 * with a model of its own (voice.commands[].model) is heard at any time. A
 * model that is missing is left out with a line in the log.
 * PCM is fed in model frames, as mipk_audio_poll does with the microphone.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mipk.h"
#include "mipk_platform_hisilicon.h"
#include "svp_acl_rt.h"
#include "asr.h"

#define WAKE_MODEL "nihaoxiaoya"
#define MAX_MODELS 8

struct keyword {
    mipk_pipeline *pipeline;
    mipk_output *output;
    const mipk_pipeline_info *info;
    int16_t *frame;
    uint32_t used;
    uint64_t pts_us;
    int kind;                /* ASR_WAKE or ASR_COMMAND */
    char text[64];           /* what asr_feed reports */
};

static mipk_hisilicon_platform *platform;
static struct keyword keywords[MAX_MODELS];
static int nkeywords;
static char diagnostic[1024];

/* Opens models/<name>.mipk as the next keyword; 0 when it is there and is a 16 kHz mono model. */
static int open_model(const char *name, int kind, const char *text)
{
    char path[96];
    snprintf(path, sizeof path, "models/%s.mipk", name);
    const mipk_pipeline_open_options options = {
        .struct_size = sizeof options,
        .max_output_handles = 1,
        .platform = mipk_platform_hisilicon_context(platform),
        .diagnostic = diagnostic,
        .diagnostic_size = sizeof diagnostic,
    };
    struct keyword *k = &keywords[nkeywords];
    mipk_status st = mipk_pipeline_open(path, &options, &k->pipeline);
    if (st != MIPK_OK) {
        printf("voice: cannot open %s: %s\n%s\n", path, mipk_status_string(st), diagnostic);
        return -1;
    }
    k->info = mipk_pipeline_get_info(k->pipeline);
    if (k->info->input_format != MIPK_FORMAT_PCM_S16LE || k->info->input_sample_rate != 16000 || k->info->input_channels != 1) {
        printf("voice: %s is not a 16 kHz mono audio model\n", path);
        return -1;
    }
    st = mipk_pipeline_output_create(k->pipeline, &k->output);
    k->frame = malloc(k->info->input_frame_samples * sizeof *k->frame);
    if (st != MIPK_OK || !k->frame) {
        printf("voice: mipk output failed: %s\n", mipk_status_string(st));
        return -1;
    }
    k->kind = kind;
    snprintf(k->text, sizeof k->text, "%s", text && text[0] ? text : k->info->name);
    nkeywords++;
    printf("voice: listening for \"%s\" (%s)\n", k->text, path);
    return 0;
}

int asr_open(const struct asr_cfg *cfg)
{
    const mipk_hisilicon_platform_options platform_options = {.struct_size = sizeof platform_options};
    mipk_status st = mipk_platform_hisilicon_create(&platform_options, &platform);
    if (st != MIPK_OK) {
        printf("voice: mipk platform failed: %s\n", mipk_status_string(st));
        return -1;
    }
    /* The NPU context is bound to the thread that first set the device, maybe the gesture thread: bind it here too. */
    svp_acl_rt_set_device(0);
    open_model(cfg->wake_model ? cfg->wake_model : WAKE_MODEL, ASR_WAKE, cfg->nwake ? cfg->wake[0] : NULL);
    for (int i = 0; i < cfg->ncommands && nkeywords < MAX_MODELS; i++) {
        if (cfg->command_models && cfg->command_models[i]) open_model(cfg->command_models[i], ASR_COMMAND, cfg->commands[i]);
    }
    return nkeywords ? 0 : -1;
}

int asr_caps(void)
{
    return ASR_CAP_WAKE;
}

void asr_touch(void)
{
}

/* Feeds one keyword's model; ASR_WAKE or ASR_COMMAND when it heard its word. */
static int feed(struct keyword *k, const int16_t *pcm, int samples, float *score)
{
    int heard = ASR_NONE;
    while (samples > 0) {
        const uint32_t room = k->info->input_frame_samples - k->used;
        const uint32_t n = (uint32_t)samples < room ? (uint32_t)samples : room;
        memcpy(k->frame + k->used, pcm, n * sizeof *pcm);
        k->used += n;
        pcm += n;
        samples -= (int)n;
        if (k->used < k->info->input_frame_samples) break;
        k->used = 0;
        const mipk_input input = mipk_input_pcm_s16(k->frame, k->info->input_frame_samples, 16000, k->pts_us);
        k->pts_us += k->info->input_frame_samples * 1000000ull / 16000;
        const mipk_status st = mipk_pipeline_run_output(k->pipeline, &input, k->output);
        if (st != MIPK_OK && st != MIPK_ERR_CAPACITY) continue; /* MIPK_AGAIN: still collecting features */
        if (mipk_output_result_count(k->output) > 0) {
            *score = mipk_output_result_at(k->output, 0)->score;
            heard = k->kind;
        }
    }
    return heard;
}

int asr_feed(const int16_t *pcm, int samples, char *text, size_t size, float *score)
{
    int heard = ASR_NONE;
    for (int i = 0; i < nkeywords; i++) {
        float s = 0;
        const int ev = feed(&keywords[i], pcm, samples, &s);
        if (ev != ASR_NONE && heard == ASR_NONE) {
            heard = ev;
            *score = s;
            snprintf(text, size, "%s", keywords[i].text);
        }
    }
    return heard;
}
