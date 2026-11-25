/*
 * pad.h on the Linux input devices: the first /dev/input/event* with gamepad
 * buttons, its sticks scaled to -1..1. A BLE pad gets there through
 * bluetoothd (BlueZ's HoG plugin and uhid), started here while there is
 * none; pairing goes through bluetoothctl.
 */
#include "pad.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define LONG_BITS (8 * sizeof(long))
#define HAS(bits, i) ((bits[(i) / LONG_BITS] >> ((i) % LONG_BITS)) & 1)

static const struct {
    int code;
    uint32_t bit;
} KEYS[] = {
    {BTN_A, PAD_A}, {BTN_B, PAD_B}, {BTN_X, PAD_X}, {BTN_Y, PAD_Y},
    {BTN_TL, PAD_LB}, {BTN_TR, PAD_RB}, {BTN_TL2, PAD_LT}, {BTN_TR2, PAD_RT},
    {BTN_START, PAD_START}, {BTN_SELECT, PAD_SELECT},
    {BTN_DPAD_UP, PAD_UP}, {BTN_DPAD_DOWN, PAD_DOWN}, {BTN_DPAD_LEFT, PAD_LEFT}, {BTN_DPAD_RIGHT, PAD_RIGHT},
};

static int fd = -1;
static int calls;            /* pad_read calls with no pad, about 50 a second */
static int bt_up;            /* bluetoothd started */
static int rx_code, ry_code; /* the right stick: Z/RZ on Bluetooth pads, RX/RY on others */
static struct input_absinfo axis[ABS_CNT];
static struct pad_state now;

static void find_pad(void)
{
    for (int i = 0; i < 32 && fd < 0; i++) {
        char path[32], name[64] = "";
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        const int f = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (f < 0) continue;
        unsigned long keys[KEY_MAX / LONG_BITS + 1] = {0}, abs[ABS_MAX / LONG_BITS + 1] = {0};
        ioctl(f, EVIOCGBIT(EV_KEY, sizeof keys), keys);
        ioctl(f, EVIOCGBIT(EV_ABS, sizeof abs), abs);
        if (!HAS(keys, BTN_GAMEPAD)) {
            close(f);
            continue;
        }
        memset(axis, 0, sizeof axis);
        for (int a = 0; a < ABS_CNT; a++) {
            if (HAS(abs, a)) ioctl(f, EVIOCGABS(a), &axis[a]);
        }
        const int z = HAS(abs, ABS_Z) && HAS(abs, ABS_RZ);
        rx_code = z ? ABS_Z : ABS_RX;
        ry_code = z ? ABS_RZ : ABS_RY;
        ioctl(f, EVIOCGNAME(sizeof name), name);
        printf("pad: %s on %s\n", name, path);
        memset(&now, 0, sizeof now);
        fd = f;
    }
}

static float scale(const struct input_event *e)
{
    const struct input_absinfo *a = &axis[e->code];
    if (a->maximum <= a->minimum) return 0;
    return 2.0f * (e->value - a->minimum) / (a->maximum - a->minimum) - 1;
}

static void event(const struct input_event *e)
{
    if (e->type == EV_KEY) {
        for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) {
            if (KEYS[i].code == e->code) now.buttons = e->value ? now.buttons | KEYS[i].bit : now.buttons & ~KEYS[i].bit;
        }
    } else if (e->type == EV_ABS && e->code < ABS_CNT) {
        if (e->code == ABS_X) now.lx = scale(e);
        else if (e->code == ABS_Y) now.ly = -scale(e);
        else if (e->code == rx_code) now.rx = scale(e);
        else if (e->code == ry_code) now.ry = -scale(e);
        else if (e->code == ABS_HAT0X)
            now.buttons = (now.buttons & ~(PAD_LEFT | PAD_RIGHT)) | (e->value < 0 ? PAD_LEFT : e->value > 0 ? PAD_RIGHT : 0);
        else if (e->code == ABS_HAT0Y)
            now.buttons = (now.buttons & ~(PAD_UP | PAD_DOWN)) | (e->value < 0 ? PAD_UP : e->value > 0 ? PAD_DOWN : 0);
    }
}

int pad_read(struct pad_state *s)
{
    if (fd < 0) {
        /* Once a second look for a pad; every 5 s start bluetoothd until it is up (at boot hci0 comes late). */
        if (calls++ % 50) return 0;
        if (!bt_up && calls % 250 == 1 && access("/usr/sbin/ble-service", X_OK) == 0)
            bt_up = system("ble-service start >/dev/null 2>&1") == 0;
        find_pad();
        if (fd < 0) return 0;
    }
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, 20) > 0) {
        struct input_event e[16];
        ssize_t n;
        while ((n = read(fd, e, sizeof e)) > 0) {
            for (size_t i = 0; i < (size_t)n / sizeof e[0]; i++) event(&e[i]);
        }
        if (n < 0 && errno != EAGAIN) {
            printf("pad: gone\n");
            close(fd);
            fd = -1;
            return 0;
        }
    }
    *s = now;
    return 1;
}

int pad_pair(char *name, size_t size)
{
    /* Up to 30 s of looking, and the first new gamepad found paired and trusted, so it connects again by itself
     * when switched on; then the gamepads paired before and not connected now are forgotten (a failed pairing
     * keeps them; a pad paired again comes back under a new address). While the scan runs, once a second, all
     * devices from BlueZ in one D-Bus call (about 0.4 s; "bluetoothctl info" takes a second for each one, and a
     * scan finds dozens): one with Icon input-gaming and Paired false. A device's Icon may come a while after
     * it is found. Lines of bluetoothctl not starting with "Device" are other devices' changes. A pad that was
     * paired before and only reconnecting (not in pairing mode) answers no pairing, and pair waits on: 30 s at
     * most (killing bluetoothctl cancels it). pair and connect
     * have no --timeout: with one bluetoothctl waits it out even when done. Pairing needs an agent registered
     * (bluetoothd refuses it with "No agent available" otherwise, and accepts it by itself with one):
     * bluetoothctl registers one only when it runs as a shell, so one runs so for as long as this does. */
    FILE *f = popen("ble-service start >/dev/null 2>&1 || exit 1\n"
                    "sleep 90 | bluetoothctl >/dev/null 2>&1 & agent=$!\n"
                    "bluetoothctl --timeout 30 scan on >/dev/null 2>&1 & scan=$!\n"
                    "trap 'kill $agent $scan 2>/dev/null' EXIT\n"
                    "new_pad() { dbus-send --system --print-reply --dest=org.bluez / org.freedesktop.DBus.ObjectManager.GetManagedObjects |\n"
                    "  awk '/^ *object path/ { if (g && u) print a; a = \"\"; g = u = 0 }\n"
                    "    k == \"Address\" { a = $NF; gsub(/\"/, \"\", a) }\n"
                    "    k == \"Icon\" && /\"input-gaming\"/ { g = 1 }\n"
                    "    k == \"Paired\" && / false$/ { u = 1 }\n"
                    "    { k = \"\" } /^ *string \"(Address|Icon|Paired)\"$/ { k = $2; gsub(/\"/, \"\", k) }\n"
                    "    END { if (g && u) print a }' | head -n 1; }\n"
                    "m=\n"
                    "while [ -z \"$m\" ] && kill -0 $scan 2>/dev/null; do sleep 1; m=$(new_pad); done\n"
                    "kill $scan 2>/dev/null; [ -n \"$m\" ] || exit 1\n"
                    "timeout 30 bluetoothctl pair $m >/dev/null 2>&1\n"
                    "bluetoothctl info $m | grep -q '^[[:space:]]*Paired: yes' || exit 1\n"
                    "bluetoothctl trust $m >/dev/null; bluetoothctl connect $m >/dev/null 2>&1\n"
                    "for o in $(bluetoothctl devices Paired | while read -r w a _; do [ \"$w\" = Device ] && echo $a; done); do\n"
                    "  [ $o = $m ] || ! bluetoothctl info $o | grep -q '^[[:space:]]*Icon: input-gaming' ||\n"
                    "    bluetoothctl info $o | grep -q '^[[:space:]]*Connected: yes' || bluetoothctl remove $o >/dev/null\n"
                    "done\n"
                    "bluetoothctl info $m | sed -n 's/^[[:space:]]*Name: //p'\n",
                    "r");
    if (!f) return -1;
    char line[128] = "";
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    line[strcspn(line, "\n")] = 0;
    snprintf(name, size, "%s", line);
    return pclose(f) == 0 && line[0] ? 0 : -1;
}

/* BlueZ keeps each device it pairs with in /var/lib/bluetooth/<adapter>/<device>/info, from the start of
 * pairing; a paired one has its keys there too ([LongTermKey], [LinkKey]...). Only gamepads are paired here. */
#define PAIRED "/var/lib/bluetooth/*/*:*/info"

int pad_paired(char *name, size_t size)
{
    glob_t g;
    int found = 0;
    if (glob(PAIRED, 0, NULL, &g) != 0) return -1;
    for (size_t i = 0; i < g.gl_pathc && !found; i++) {
        FILE *f = fopen(g.gl_pathv[i], "r");
        if (!f) continue;
        char line[128], dev[128] = "?";
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = 0;
            if (!strncmp(line, "Name=", 5)) snprintf(dev, sizeof dev, "%s", line + 5);
            if (line[0] == '[' && strstr(line, "Key]")) found = 1;
        }
        fclose(f);
        /* The address is the directory's name: .../<adapter>/<address>/info. */
        *strrchr(g.gl_pathv[i], '/') = 0;
        snprintf(name, size, "%s (%s)", strrchr(g.gl_pathv[i], '/') + 1, dev);
    }
    globfree(&g);
    if (!found) name[0] = 0;
    return found ? 0 : -1;
}

int pad_unpair(void)
{
    /* bluetoothctl remove disconnects it too, and deletes its directory. */
    return system("ble-service start >/dev/null 2>&1 || exit 1\n"
                  "for d in " PAIRED "; do [ -e \"$d\" ] || continue; d=${d%/info}; bluetoothctl remove ${d##*/} >/dev/null 2>&1; done\n"
                  "! ls " PAIRED " >/dev/null 2>&1") == 0 ? 0 : -1;
}

const char *pad_missing(void)
{
    if (access("/usr/sbin/ble-service", X_OK) != 0) return "no_ble";
    /* uhid makes /dev/uhid once loaded; S51wireless loads it with the Bluetooth stack, in the background at
     * boot, often after this runs: its module is enough. */
    if (access("/dev/uhid", F_OK) != 0 && access("/drv/ko/extdrv/uhid.ko", F_OK) != 0) return "no_uhid";
    /* The HoG plugin is built into bluetoothd: its name is in the program. */
    FILE *f = fopen("/usr/libexec/bluetooth/bluetoothd", "rb");
    if (!f) return "no_ble";
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    char *b = malloc(n > 0 ? n : 1);
    rewind(f);
    const size_t got = b ? fread(b, 1, n, f) : 0;
    fclose(f);
    const int hog = got && memmem(b, got, "input-hog", 9);
    free(b);
    return hog ? NULL : "no_hog";
}
