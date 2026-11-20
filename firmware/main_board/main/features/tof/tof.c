/*
 * The ToF feature: an ST VL53L5CX (8x8 zones, up to some 4 m) on the I2C bus
 * Kconfig names (CONFIG_DOITDUCK_TOF_I2C; its pins are muxed with the bus's at
 * start). A thread uploads the sensor's firmware (some 84 KB, a few seconds),
 * then reads all 64 zones 10 times a second; robot.status carries them for the
 * web page.
 *
 * The ToF must never get in the way of walking. On the Cove-610 the I2C driver
 * polls with interrupts off (the device tree gives I2C1 no interrupt), on the
 * CPU that runs the transfer. So the thread runs on CPU 1, away from the servo
 * UART's interrupt on CPU 0, and the platform moves 32 bytes a transfer (some
 * 1 ms each). An access that gets no answer holds that CPU until the driver
 * gives up: some 10 ms with V610-Buildroot's patch 0015 (155 ms without), and
 * the driver's error goes to the serial console. So a sensor that is missing or
 * stops answering (unplugged) is looked for again every PROBE_S only while the
 * duck is relaxed (no torque: nothing to fall) and nothing plays; one access
 * first, so a missing sensor fails once. Plugged in, its firmware loads again
 * (some 5 s).
 *
 * Zones come as the ULD gives them: zone 0 first, rows of 8. Seen from the
 * sensor, the lens mirrors the picture.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "duck.h"
#include "feature.h"
#include "osal.h"
#include "sound.h"
#include "vl53l5cx_api.h"

#define ZONES 64
#define HZ 10
#define PROBE_S 2

static VL53L5CX_Configuration dev;
static int16_t mm[ZONES];          /* -1: nothing measured there */
static int ok, nearest = -1;
static os_mutex_t lock = OS_MUTEX_INIT;

static int configure(const cJSON *section)
{
    (void)section;
    if (access(CONFIG_DOITDUCK_TOF_I2C, R_OK | W_OK) != 0) {
        printf("tof: no %s\n", CONFIG_DOITDUCK_TOF_I2C);
        return -1;
    }
    return 0;
}

/* Firmware, 8x8 zones at HZ, continuous ranging; 0 when ranging. One write first: a missing sensor stalls once. */
static int setup(void)
{
    uint8_t alive = 0;
    if (WrByte(&dev.platform, 0x7fff, 0x00) != 0) return -1;
    if (vl53l5cx_is_alive(&dev, &alive) != 0 || !alive) return -1;
    const double t0 = os_now();
    if (vl53l5cx_init(&dev) != 0 || vl53l5cx_set_resolution(&dev, VL53L5CX_RESOLUTION_8X8) != 0 ||
        vl53l5cx_set_ranging_frequency_hz(&dev, HZ) != 0 ||
        vl53l5cx_set_ranging_mode(&dev, VL53L5CX_RANGING_MODE_CONTINUOUS) != 0 || vl53l5cx_start_ranging(&dev) != 0)
        return -1;
    printf("tof: VL53L5CX ranging, 8x8 at %d Hz (firmware loaded in %.1f s)\n", HZ, os_now() - t0);
    return 0;
}

static void task(void *arg)
{
    (void)arg;
    static VL53L5CX_ResultsData r;
    dev.platform.address = VL53L5CX_DEFAULT_I2C_ADDRESS;
    dev.platform.fd = open(CONFIG_DOITDUCK_TOF_I2C, O_RDWR);
    int told = 0; /* "not answering" said once, not at every look */
    for (int first = 1;; first = 0) {
        if (!first) {
            os_sleep(PROBE_S);
            os_lock(&d.lock);
            const int relaxed = !d.enabled && (d.mode == MODE_OFF || d.mode == MODE_PREVIEW);
            os_unlock(&d.lock);
            if (!relaxed || sound_playing()) continue;
        }
        if (setup() != 0) {
            if (!told) printf("tof: no VL53L5CX answering on %s; looked for every %d s while relaxed\n",
                              CONFIG_DOITDUCK_TOF_I2C, PROBE_S);
            told = 1;
            continue;
        }
        told = 0;
        os_lock(&lock);
        ok = 1;
        os_unlock(&lock);
        for (;;) {
            uint8_t ready = 0;
            if (vl53l5cx_check_data_ready(&dev, &ready) == 0 && !ready) {
                os_sleep(0.01);
                continue;
            }
            if (!ready || vl53l5cx_get_ranging_data(&dev, &r) != 0) break;
            /* Status 5 is a good measure; 9 too (with a wide pulse). Anything else is not trusted. */
            int16_t z[ZONES];
            int near = -1;
            for (int i = 0; i < ZONES; i++) {
                const int good = r.nb_target_detected[i] > 0 && (r.target_status[i] == 5 || r.target_status[i] == 9);
                z[i] = good ? r.distance_mm[i] : -1;
                if (good && (near < 0 || z[i] < near)) near = z[i];
            }
            os_lock(&lock);
            memcpy(mm, z, sizeof mm);
            nearest = near;
            os_unlock(&lock);
        }
        printf("tof: the VL53L5CX stopped answering (unplugged?)\n");
        os_lock(&lock);
        ok = 0;
        nearest = -1;
        os_unlock(&lock);
    }
}

static void start(void)
{
    os_thread(task, NULL, 0, 1);
}

static void status(cJSON *r)
{
    int z[ZONES];
    os_lock(&lock);
    cJSON_AddBoolToObject(r, "ok", ok);
    cJSON_AddNumberToObject(r, "nearest", nearest);
    for (int i = 0; i < ZONES; i++) z[i] = ok ? mm[i] : -1;
    os_unlock(&lock);
    cJSON_AddItemToObject(r, "mm", cJSON_CreateIntArray(z, ZONES));
}

const struct feature tof_feature = {
    .name = "tof",
    .configure = configure,
    .start = start,
    .status = status,
};
