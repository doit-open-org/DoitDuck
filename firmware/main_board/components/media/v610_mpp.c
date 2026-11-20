/*
 * audio.h, camera.h and hand.h on the Cove-610 through media_engine (MPP): the
 * MIC input, the speaker and the camera in one context. Audio 16 kHz mono,
 * 10 ms frames, channels and gains from audio_configure; the camera as 720p
 * H.264 for the web page, and a second, model-sized stream for the hand model
 * (gestures) when models/mediapipe_hand.mipk is there. Opened on first use by the audio and retried by the
 * caller: sdk_server, which MPP needs, starts after the app at boot. With no
 * camera, audio works alone.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "audio.h"
#include "camera.h"
#include "hand.h"
#include "media_engine/media.h"
#include "mipk_video.h"
#include "svp_acl_rt.h"
#include "osal.h"

#define FRAME_SAMPLES 160
#define VIDEO_STREAM 0
#define HAND_STREAM 1
#define HAND_MODEL "models/mediapipe_hand.mipk"
#define SDK_SOCKET "/run/sdk_server/osal_uds_server.sock"

static struct audio_cfg cfg;
static media_context *media;
static mipk_video *hand_model;
static int hand_bound;
static int video_ok, closed;
static int no_camera; /* none answered: the engine runs audio only */
/* The engine is open, for the threads that play: only audio_read (the microphone's thread) opens it,
 * as two threads opening it at once hang in the SDK. */
static int audio_ok;

static void close_audio(void);

/*
 * MPP belongs to sdk_server, which starts after the app at boot. Touching MPP or the NPU
 * before it is up makes this process set up the MPP system itself; sdk_server then fails,
 * and this process crashes on its first call to it. Waits up to wait_s for its socket.
 */
static int sdk_ready(double wait_s)
{
    const double t0 = os_now();
    while (access(SDK_SOCKET, F_OK) != 0) {
        if (os_now() - t0 >= wait_s) return 0;
        os_sleep(0.5);
    }
    return 1;
}

static int open_audio(void)
{
    if (closed) return -1; /* exiting: the voice thread must not open it again */
    if (!sdk_ready(0)) return -1; /* the caller tries again */
    media_config config;
    media_config_init(&config);
    media_audio_input_config_init(&config.audio_input);
    media_audio_output_config_init(&config.audio_output);
    config.audio_input.physical_channel = (unsigned)cfg.mic_channel;
    config.audio_input.gain_db = cfg.mic_gain_db;
    config.audio_input.frame_samples = FRAME_SAMPLES;
    /* Echo cancelling (MPP VQE TALK), so the wake word is heard while xiaozhi speaks. */
    config.audio_input.enable_aec = true;
    config.audio_output.gain_db = cfg.speaker_gain_db;
    /* Small enough for Wi-Fi; a key frame each second, so a new viewer starts at once. */
    media_video_config_init(&config.video);
    config.video.streams[VIDEO_STREAM].width = 1280;
    config.video.streams[VIDEO_STREAM].height = 720;
    config.video.streams[VIDEO_STREAM].frame_rate = 15;
    config.video.streams[VIDEO_STREAM].bit_rate_kbps = 1200;
    config.video.streams[VIDEO_STREAM].gop = 15;
    /* The stream takes the hand model's input size, when hand_open has loaded it. */
    /* 10 recognition frames a second leave the CPU to the control loops. */
    if (hand_model &&mipk_video_configure_stream(hand_model, &config.video.streams[HAND_STREAM], 0, 10,
                                                  MEDIA_VIDEO_OUTPUT_FRAME) == MIPK_VIDEO_OK)
        config.video.stream_count = 2;
    media_status st = media_engine_open(&config, &media);
    if (st == MEDIA_OK) st = media_audio_input_open(media);
    if (st == MEDIA_OK) st = media_audio_output_open(media);
    if (st != MEDIA_OK) {
        printf("audio: open failed: %s\n", media_status_string(st));
        if (media) media_engine_close(media);
        media = NULL;
        return -1;
    }
    if (no_camera) {
        __atomic_store_n(&audio_ok, 1, __ATOMIC_RELEASE);
        return 0;
    }
    /* Like the audio, the camera's first open after another process has used it fails; it opens on a retry. */
    for (int i = 0; i < 3; i++) {
        if (i) os_sleep(1);
        st = media_video_open(media);
        if (st == MEDIA_OK) break;
    }
    video_ok = st == MEDIA_OK;
    if (!video_ok) printf("camera: open failed: %s\n", media_status_string(st));
    /*
     * With no camera connected it opens all the same, then never gives a picture while the
     * SDK logs every try for one (some 2 KB/s): wait for a first picture. If none comes, the
     * whole engine closes and the caller opens it again without the camera: closing the
     * video alone leaves the ISP thread running, and it stalls the servo bus (feedback
     * 100 -> 80 Hz, servo replies lost).
     */
    media_video_packet packet;
    if (video_ok && media_video_read_packet(media, VIDEO_STREAM, 3000, &packet) == MEDIA_OK) {
        media_video_packet_release(&packet);
    } else if (video_ok) {
        printf("camera: no picture in 3 s (not connected?); camera off\n");
        no_camera = 1;
        close_audio();
        return -1;
    }
    __atomic_store_n(&audio_ok, 1, __ATOMIC_RELEASE);
    return 0;
}

static void close_audio(void)
{
    __atomic_store_n(&audio_ok, 0, __ATOMIC_RELEASE);
    if (video_ok) media_video_close(media);
    video_ok = 0;
    media_audio_output_close(media);
    media_audio_input_close(media);
    media_engine_close(media);
    media = NULL;
}

void audio_configure(const struct audio_cfg *c)
{
    cfg = *c;
}

int audio_read(int16_t *pcm, int samples)
{
    if (!media && open_audio() != 0) return -1;
    size_t count = 0;
    uint64_t pts = 0;
    const media_status st = media_audio_input_read(media, pcm, (size_t)samples, 1000, &count, &pts);
    if (st == MEDIA_ERR_TIMEOUT || st == MEDIA_AGAIN) return 0;
    if (st != MEDIA_OK) {
        printf("audio: read failed: %s\n", media_status_string(st));
        close_audio();
        return -1;
    }
    return (int)count;
}

int audio_play(const int16_t *pcm, int samples, int rate)
{
    if (rate != 16000 || !__atomic_load_n(&audio_ok, __ATOMIC_ACQUIRE)) return -1;
    const media_status st = media_audio_play_memory(media, MEDIA_AUDIO_FORMAT_PCM_S16LE, pcm, (size_t)samples * 2);
    if (st != MEDIA_OK) printf("audio: play failed: %s\n", media_status_string(st));
    return st == MEDIA_OK ? 0 : -1;
}

int audio_stream(const int16_t *pcm, int samples)
{
    if (!__atomic_load_n(&audio_ok, __ATOMIC_ACQUIRE)) return -1;
    const media_status st = media_audio_output_write(media, pcm, (size_t)samples, 1000);
    if (st != MEDIA_OK) printf("audio: stream failed: %s\n", media_status_string(st));
    return st == MEDIA_OK ? 0 : -1;
}

void audio_flush(void)
{
    if (__atomic_load_n(&audio_ok, __ATOMIC_ACQUIRE)) media_audio_play_stop(media);
}

int camera_h264_read(uint8_t *buf, int size, int *key_frame)
{
    if (!media || !video_ok) return -1;
    media_video_packet packet = {0};
    const media_status st = media_video_read_packet(media, VIDEO_STREAM, 200, &packet);
    if (st == MEDIA_ERR_TIMEOUT || st == MEDIA_AGAIN) return 0;
    if (st != MEDIA_OK) return -1;
    const int n = packet.size <= (size_t)size ? (int)packet.size : 0; /* a frame too big for buf is dropped */
    memcpy(buf, packet.data, (size_t)n);
    *key_frame = packet.key_frame;
    media_video_packet_release(&packet);
    return n;
}

void camera_key_frame(void)
{
    if (media && video_ok) media_video_request_key_frame(media, VIDEO_STREAM);
}

int camera_jpeg(uint8_t *buf, int size)
{
    static const char path[] = "/tmp/doitduck_photo.jpg";
    if (!media || !video_ok || media_video_save_jpeg(media, VIDEO_STREAM, path, 2000) != MEDIA_OK) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    const size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    remove(path);
    return n > 0 && n < (size_t)size ? (int)n : -1;
}

int hand_open(void)
{
    if (!sdk_ready(60)) {
        printf("gesture: sdk_server is not running; gestures off\n");
        return -1;
    }
    mipk_video_config mc;
    mipk_video_config_init(&mc);
    mc.package_path = HAND_MODEL;
    const mipk_video_status ms = mipk_video_open(&mc, &hand_model);
    if (ms != MIPK_VIDEO_OK) {
        printf("gesture: %s: %s\n", HAND_MODEL, mipk_video_status_string(ms));
        hand_model = NULL;
        return -1;
    }
    return 0;
}

int hand_read(struct hand *hands, int max)
{
    if (!media || !video_ok || !hand_model) return -1;
    /* The NPU context is bound to the thread that set the device; bind the reading thread too. */
    if (!hand_bound) {
        svp_acl_rt_set_device(0);
        hand_bound = 1;
    }
    mipk_video_event ev;
    const mipk_video_status st = mipk_video_process(hand_model, media, HAND_STREAM, 200, &ev);
    if (st == MIPK_VIDEO_AGAIN) return 0;
    if (st != MIPK_VIDEO_OK) return -1;
    /* Results come as a box and 21 key points per hand, tied together by parent_id. */
    uint64_t ids[4];
    int points[4] = {0}, n = 0;
    if (max > 4) max = 4;
    for (size_t i = 0; i < mipk_output_result_count(ev.output); i++) {
        const mipk_result *r = mipk_output_result_at(ev.output, i);
        if (!r || r->type != MIPK_RESULT_KEYPOINT) continue;
        const int k = r->value.keypoint.keypoint_id;
        if (k < 0 || k >= HAND_POINTS) continue;
        int h = 0;
        while (h < n && ids[h] != r->parent_id) h++;
        if (h == n) {
            if (n == max) continue;
            ids[n++] = r->parent_id;
        }
        hands[h].x[k] = r->value.keypoint.x / (float)ev.width;
        hands[h].y[k] = r->value.keypoint.y / (float)ev.height;
        points[h]++;
    }
    /* Keep only whole hands. */
    int whole = 0;
    for (int h = 0; h < n; h++) {
        if (points[h] == HAND_POINTS) hands[whole++] = hands[h];
    }
    return whole;
}

void camera_lines(const float (*seg)[4], int n, uint32_t rgb)
{
    if (!media || !video_ok) return;
    media_video_line lines[MEDIA_VIDEO_MAX_LINES];
    if (n > (int)MEDIA_VIDEO_MAX_LINES) n = MEDIA_VIDEO_MAX_LINES;
    for (int i = 0; i < n; i++) {
        lines[i] = (media_video_line){.x1 = seg[i][0], .y1 = seg[i][1], .x2 = seg[i][2], .y2 = seg[i][3],
                                      .red = (uint8_t)(rgb >> 16), .green = (uint8_t)(rgb >> 8), .blue = (uint8_t)rgb,
                                      .thickness = 3};
    }
    media_video_set_lines(media, VIDEO_STREAM, lines, (size_t)n);
}

void audio_close(void)
{
    closed = 1;
    __atomic_store_n(&audio_ok, 0, __ATOMIC_RELEASE);
    if (!media) return;
    media_context *m = media;
    const int v = video_ok;
    /* The voice and video threads see no media from now on; let a read already inside return first. */
    media = NULL;
    video_ok = 0;
    os_sleep(0.3);
    if (v) media_video_close(m);
    media_audio_output_close(m);
    media_audio_input_close(m);
    media_engine_close(m);
    mipk_video_close(hand_model);
    hand_model = NULL;
}
