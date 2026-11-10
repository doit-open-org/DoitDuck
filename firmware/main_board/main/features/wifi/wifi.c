/*
 * The Wi-Fi feature: the web page's Wi-Fi settings, so a new board can be put
 * on a network. wifi.status and wifi.scan read wpa_supplicant (wpa_cli);
 * wifi.connect writes the network to CONF, which keeps it for the next boot
 * (S52wifi starts wpa_supplicant and a DHCP client then; see
 * tools/board/S52wifi), and has wpa_supplicant read it again now, starting it
 * when it is not running, and the DHCP client renew once associated.
 *
 * wifi.ap opens an open hotspot, DoitDuck-<end of the MAC>, for setting the
 * network up without a cable (the voice command "开始配网" calls it): a second
 * interface, AP_IF, that the wpa_supplicant on IFACE runs as an access point
 * (made through its global control socket, GLOBAL) at AP_IP with a small DHCP
 * server; the web page is at http://AP_IP/, and IFACE stays on its network.
 * Its DNS server answers every name with AP_IP and httpd redirects those names
 * to the page, so a phone joining the hotspot opens it (a captive portal).
 * It closes on wifi.ap {on: false} (the page sends it once the new address has
 * been read), or AP_S seconds after the last use when IFACE is on a network;
 * with wifi.ap {always: true} (kept across restarts) it stays open, for use
 * away from any network. One thread owns the hotspot from start to end.
 */
#define _DEFAULT_SOURCE /* popen, kill, fileno, SO_BINDTODEVICE */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "feature.h"
#include "osal.h"
#include "storage.h"

#define IFACE "wlan0"
#define CONF "/etc/wireless/wpa_supplicant.conf"
#define DHCP_PID "/var/run/wlan0-udhcpc.pid"
#define GLOBAL "/var/run/wpa_global"
#define AP_IF "ap0"
#define AP_CONF "/tmp/wpa_ap.conf"
#define AP_IP "192.168.4.1"
#define AP_S 600
#define MAX_NETS 24
#define MAX_LEASES 16

struct net {
    char ssid[64];
    int signal, secure;
};

static os_mutex_t lock = OS_MUTEX_INIT;
static int ap_on, ap_leave, ap_always;  /* the hotspot is up; it is to close; it stays open */
static double ap_until;
static char ap_ssid[32];

static int configure(const cJSON *section)
{
    (void)section;
    if (access("/sys/class/net/" IFACE, F_OK) != 0) {
        printf("wifi: no " IFACE "\n");
        return -1;
    }
    return 0;
}

/* wpa_cli prints bytes it cannot show as \xNN, and \\ and \" escaped: the bytes back. */
static void unescape(const char *in, char *out, size_t size)
{
    size_t n = 0;
    while (*in && n + 1 < size) {
        unsigned v;
        if (in[0] == '\\' && in[1] == 'x' && sscanf(in + 2, "%2x", &v) == 1) {
            out[n++] = (char)v;
            in += 4;
        } else if (in[0] == '\\' && in[1]) {
            out[n++] = in[1];
            in += 2;
        } else {
            out[n++] = *in++;
        }
    }
    out[n] = 0;
}

/* One wpa_cli command; its output in out, 0 when it ran. */
static int wpa_cli(const char *args, char *out, size_t size)
{
    char cmd[128];
    snprintf(cmd, sizeof cmd, "wpa_cli -i " IFACE " %s 2>/dev/null", args);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    const size_t n = fread(out, 1, size - 1, f);
    out[n] = 0;
    return pclose(f) == 0 ? 0 : -1;
}

/* The value of a key=value line of wpa_cli status, or "". */
static void field(const char *text, const char *key, char *out, size_t size)
{
    const size_t k = strlen(key);
    out[0] = 0;
    for (const char *line = text; *line; line += strcspn(line, "\n") + (line[strcspn(line, "\n")] != 0)) {
        if (strncmp(line, key, k) == 0 && line[k] == '=') {
            char raw[128];
            snprintf(raw, sizeof raw, "%.*s", (int)strcspn(line + k + 1, "\n"), line + k + 1);
            unescape(raw, out, size);
            return;
        }
    }
}

/* The networks of the last scan, strongest first, each once. */
static int scan_results(struct net *nets)
{
    static char text[8192];
    int n = 0;
    if (wpa_cli("scan_results", text, sizeof text) != 0) return 0;
    /* After a header line: bssid, frequency, signal level, flags, ssid, split by tabs (a hidden ssid is empty). */
    for (char *line = strchr(text, '\n'); line && *++line; line = strchr(line, '\n')) {
        char buf[256], *f[5] = {buf}, ssid[64];
        int nf = 1;
        snprintf(buf, sizeof buf, "%.*s", (int)strcspn(line, "\n"), line);
        for (char *c = buf; *c && nf < 5; c++) {
            if (*c == '\t') {
                *c = 0;
                f[nf++] = c + 1;
            }
        }
        if (nf < 5) continue;
        const int signal = atoi(f[2]);
        const char *flags = f[3];
        unescape(f[4], ssid, sizeof ssid);
        if (!ssid[0]) continue; /* hidden */
        int k = 0;
        while (k < n && strcmp(nets[k].ssid, ssid)) k++;
        if (k == n && n == MAX_NETS) continue;
        if (k == n) {
            snprintf(nets[n].ssid, sizeof nets[n].ssid, "%s", ssid);
            nets[n++].signal = -1000;
        }
        if (signal > nets[k].signal) nets[k].signal = signal;
        nets[k].secure = strstr(flags, "WPA") || strstr(flags, "SAE") || strstr(flags, "WEP");
    }
    for (int i = 1; i < n; i++) { /* strongest first */
        for (int j = i; j > 0 && nets[j].signal > nets[j - 1].signal; j--) {
            const struct net t = nets[j];
            nets[j] = nets[j - 1];
            nets[j - 1] = t;
        }
    }
    return n;
}

/* {state, ssid, ip, ap, ap_ssid, ap_always}: state is wpa_supplicant's (COMPLETED when on a network), or "off". */
static const char *wifi_status(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)p;
    static char text[2048];
    char state[32] = "off", ssid[64] = "", ip[32] = "";
    os_lock(&lock);
    const int on = ap_on, always = ap_always;
    os_unlock(&lock);
    if (wpa_cli("status", text, sizeof text) == 0) {
        field(text, "wpa_state", state, sizeof state);
        field(text, "ssid", ssid, sizeof ssid);
        field(text, "ip_address", ip, sizeof ip);
    }
    cJSON_AddStringToObject(r, "state", state);
    cJSON_AddStringToObject(r, "ssid", ssid);
    cJSON_AddStringToObject(r, "ip", ip);
    cJSON_AddBoolToObject(r, "ap", on);
    cJSON_AddStringToObject(r, "ap_ssid", ap_ssid);
    cJSON_AddBoolToObject(r, "ap_always", always);
    return NULL;
}

/* The networks found last; and a new scan for next time. */
static const char *wifi_scan(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)p;
    struct net nets[MAX_NETS];
    char ok[16];
    wpa_cli("scan", ok, sizeof ok);
    const int n = scan_results(nets);
    cJSON *list = cJSON_AddArrayToObject(r, "networks");
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", nets[i].ssid);
        cJSON_AddNumberToObject(e, "signal", nets[i].signal);
        cJSON_AddBoolToObject(e, "secure", nets[i].secure);
        cJSON_AddItemToArray(list, e);
    }
    return NULL;
}

/* Once associated (up to 30 s), a new lease from the DHCP client. */
static void renew(void *arg)
{
    (void)arg;
    char text[1024];
    for (int i = 0; i < 60; i++) {
        os_sleep(0.5);
        if (wpa_cli("status", text, sizeof text) == 0 && strstr(text, "wpa_state=COMPLETED")) break;
    }
    FILE *f = fopen(DHCP_PID, "r");
    int pid = 0;
    if (f) {
        if (fscanf(f, "%d", &pid) != 1) pid = 0;
        fclose(f);
    }
    if (pid > 0 && kill(pid, SIGUSR1) == 0) return; /* udhcpc: renew */
    if (system("udhcpc -i " IFACE " -b -S -p " DHCP_PID " >/dev/null 2>&1") != 0) printf("wifi: udhcpc failed\n");
}

/* wpa_supplicant on CONF's network, started if it is not running (with GLOBAL, for the hotspot); the DHCP client renews once associated. */
static int join(void)
{
    char out[64];
    if (wpa_cli("reconfigure", out, sizeof out) != 0 &&
        system("ifconfig " IFACE " up && wpa_supplicant -B -i " IFACE " -Dnl80211 -c " CONF " -g " GLOBAL " >/dev/null 2>&1") != 0)
        return -1;
    os_thread(renew, NULL, 0, -1);
    return 0;
}

/* {ssid, password}: an open network without a password, else 8 to 63 characters. */
static const char *wifi_connect(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    (void)r;
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(p, "ssid");
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(p, "password");
    const char *pw = cJSON_IsString(k) ? k->valuestring : "";
    if (!cJSON_IsString(s) || !s->valuestring[0] || strlen(s->valuestring) > 32) return "wifi_ssid";
    const size_t len = strlen(pw);
    if (len && (len < 8 || len > 63)) return "wifi_password";
    for (const char *c = pw; *c; c++) {
        if (*c < 0x20 || *c > 0x7e) return "wifi_password";
    }
    /* The name as hex, so any bytes go; the password quoted (it may hold quotes: the last one ends it). */
    char hex[65] = "";
    for (const unsigned char *c = (const unsigned char *)s->valuestring; *c; c++) sprintf(hex + strlen(hex), "%02x", *c);
    FILE *f = fopen(CONF ".new", "w");
    if (!f) return "wifi_write";
    fprintf(f, "ctrl_interface=/var/run/wpa_supplicant\nupdate_config=1\ncountry=CN\n\nnetwork={\n    ssid=%s\n", hex);
    if (len) fprintf(f, "    psk=\"%s\"\n", pw);
    else fprintf(f, "    key_mgmt=NONE\n");
    fprintf(f, "}\n");
    const int bad = fflush(f) != 0 || fsync(fileno(f)) != 0;
    fclose(f);
    if (bad || rename(CONF ".new", CONF) != 0) return "wifi_write";
    printf("wifi: connecting to %s\n", s->valuestring);
    os_lock(&lock);
    const int on = ap_on;
    if (on) ap_until = os_now() + AP_S;
    os_unlock(&lock);
    /* The page on the hotspot waits for the address on the new network: none until DHCP has answered there. */
    if (on && system("kill $(cat " DHCP_PID ") 2>/dev/null; ifconfig " IFACE " 0.0.0.0") != 0) printf("wifi: ifconfig failed\n");
    if (join() != 0) return "wifi_write";
    return NULL;
}

/* ---- the hotspot ---- */

/* A DHCP reply (OFFER or ACK) to a request, the address AP_IP's subnet .10 + lease. */
static int dhcp_reply(const uint8_t *q, uint8_t *a, int type, int lease)
{
    memset(a, 0, 240);
    a[0] = 2;                                 /* BOOTREPLY */
    a[1] = 1;                                 /* Ethernet */
    a[2] = 6;
    memcpy(a + 4, q + 4, 4);                  /* xid */
    memcpy(a + 10, q + 10, 2);                /* flags */
    const uint8_t yi[4] = {192, 168, 4, (uint8_t)(10 + lease)}, si[4] = {192, 168, 4, 1};
    memcpy(a + 16, yi, 4);
    memcpy(a + 20, si, 4);
    memcpy(a + 28, q + 28, 16);               /* chaddr */
    memcpy(a + 236, q + 236, 4);              /* magic cookie */
    uint8_t *o = a + 240;
    const uint8_t opts[] = {53, 1, (uint8_t)type, 54, 4, 192, 168, 4, 1, 51, 4, 0, 0, 0x0e, 0x10,
                            1, 4, 255, 255, 255, 0, 3, 4, 192, 168, 4, 1, 6, 4, 192, 168, 4, 1, 255};
    memcpy(o, opts, sizeof opts);
    return 240 + (int)sizeof opts;
}

/* A command to wpa_supplicant's global control socket; 0 when it answered OK. */
static int global(const char *args)
{
    char cmd[256], out[16] = "";
    snprintf(cmd, sizeof cmd, "wpa_cli -g " GLOBAL " %s 2>/dev/null", args);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    if (!fgets(out, sizeof out, f)) out[0] = 0;
    pclose(f);
    return strncmp(out, "OK", 2) == 0 ? 0 : -1;
}

/* The hotspot, from opening to closing: AP_IF and its DHCP server until it is to close. */
static void hotspot(void *arg)
{
    (void)arg;
    char out[1024];
    /* At boot wpa_supplicant may not be running yet; one started without GLOBAL (an older S52wifi) starts again with it. */
    for (int i = 0; i < 30 && access(GLOBAL, F_OK) != 0; i++) os_sleep(1);
    if (access(GLOBAL, F_OK) != 0) {
        wpa_cli("terminate", out, sizeof out);
        os_sleep(1);
        join();
        os_sleep(1);
    }
    FILE *f = fopen(AP_CONF, "w");
    if (f) {
        fprintf(f, "ctrl_interface=/var/run/wpa_supplicant\n\nnetwork={\n    ssid=\"%s\"\n    mode=2\n"
                   "    frequency=2437\n    key_mgmt=NONE\n}\n", ap_ssid);
        fclose(f);
    }
    global("interface_remove " AP_IF); /* left by a program that stopped with the hotspot open */
    int bad = !f || global("interface_add " AP_IF " " AP_CONF " nl80211 /var/run/wpa_supplicant \"\" \"\" create ap") != 0;
    os_sleep(1);
    if (!bad) bad = system("ifconfig " AP_IF " " AP_IP " netmask 255.255.255.0 up") != 0;
    /* Up once the radio runs it: a radio that refuses leaves AP_IF DISCONNECTED ("Failed to start AP" in dmesg). */
    int up = 0;
    for (int i = 0; i < 10 && !bad && !up; i++) {
        os_sleep(0.5);
        up = system("wpa_cli -i " AP_IF " status 2>/dev/null | grep -q wpa_state=COMPLETED") == 0;
    }
    if (bad || !up) {
        printf("wifi: the hotspot did not start\n");
        global("interface_remove " AP_IF);
        os_lock(&lock);
        ap_on = 0;
        os_unlock(&lock);
        return;
    }
    printf("wifi: hotspot %s up, http://" AP_IP "/\n", ap_ssid);

    const int sock = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    const int one = 1;
    struct sockaddr_in me = {.sin_family = AF_INET, .sin_port = htons(67), .sin_addr.s_addr = htonl(INADDR_ANY)};
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, AP_IF, sizeof AP_IF);
    if (sock < 0 || bind(sock, (struct sockaddr *)&me, sizeof me) != 0) printf("wifi: no DHCP server for the hotspot (%m)\n");
    const int dns = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    const struct sockaddr_in dns_me = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = inet_addr(AP_IP)};
    if (dns < 0 || bind(dns, (const struct sockaddr *)&dns_me, sizeof dns_me) != 0) printf("wifi: no DNS server for the hotspot (%m)\n");
    uint8_t leases[MAX_LEASES][6];
    int nleases = 0;
    double checked = 0; /* when IFACE's state was last looked at */
    for (;;) {
        os_lock(&lock);
        const int leave = ap_leave, late = !ap_always && os_now() > ap_until;
        os_unlock(&lock);
        if (leave) break;
        if (late && os_now() - checked > 1) {
            checked = os_now();
            if (wpa_cli("status", out, sizeof out) == 0 && strstr(out, "wpa_state=COMPLETED") && strstr(out, "ip_address=")) break;
        }
        struct pollfd fds[2] = {{.fd = sock, .events = POLLIN}, {.fd = dns, .events = POLLIN}};
        poll(fds, 2, 1000);
        if (fds[1].revents & POLLIN) {
            /* One question: for an A record the answer is AP_IP, for any other type there is none. */
            uint8_t d[512];
            struct sockaddr_in from;
            socklen_t from_len = sizeof from;
            const ssize_t dn = recvfrom(dns, d, sizeof d - 16, 0, (struct sockaddr *)&from, &from_len);
            size_t end = 12;
            while (end < (size_t)dn && dn > 12 && d[end]) end += 1u + d[end];
            if (dn > 12 && end + 5 <= (size_t)dn && !(d[2] & 0x80) && d[4] == 0 && d[5] == 1) {
                const int is_a = d[end + 1] == 0 && d[end + 2] == 1;
                const uint8_t answer[16] = {0xc0, 12, 0, 1, 0, 1, 0, 0, 0, 10, 0, 4, 192, 168, 4, 1};
                end += 5;
                d[2] = 0x81;
                d[3] = 0x80;
                memset(d + 6, 0, 6);
                d[7] = (uint8_t)is_a;
                memcpy(d + end, answer, sizeof answer);
                sendto(dns, d, end + (is_a ? sizeof answer : 0), 0, (struct sockaddr *)&from, from_len);
            }
        }
        if (!(fds[0].revents & POLLIN)) continue;
        uint8_t q[576], a[300];
        const ssize_t len = recv(sock, q, sizeof q, 0);
        if (len < 244 || q[0] != 1 || q[236] != 99 || q[237] != 130 || q[238] != 83 || q[239] != 99) continue;
        int type = 0;
        for (int i = 240; i + 2 < len && q[i] != 255; i += q[i] ? 2 + q[i + 1] : 1) {
            if (q[i] == 53) type = q[i + 2];
        }
        if (type != 1 && type != 3) continue; /* DISCOVER, REQUEST */
        int lease = 0;
        while (lease < nleases && memcmp(leases[lease], q + 28, 6)) lease++;
        if (lease == nleases) {
            if (nleases == MAX_LEASES) continue;
            memcpy(leases[nleases++], q + 28, 6);
        }
        const int size = dhcp_reply(q, a, type == 1 ? 2 : 5, lease);
        struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(68), .sin_addr.s_addr = htonl(INADDR_BROADCAST)};
        sendto(sock, a, (size_t)size, 0, (struct sockaddr *)&to, sizeof to);
    }
    if (sock >= 0) close(sock);
    if (dns >= 0) close(dns);
    global("interface_remove " AP_IF);
    printf("wifi: hotspot closed\n");
    os_lock(&lock);
    ap_on = ap_leave = 0;
    os_unlock(&lock);
}

/* {on, always}: {on: true} opens the hotspot (or keeps it open AP_S more seconds), {on: false} closes it;
 * {always: true} opens it to stay open, across restarts too, {always: false} closes it. */
static const char *wifi_ap(int src, const cJSON *p, cJSON *r)
{
    (void)src;
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(p, "always");
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(p, "on");
    const int on = cJSON_IsBool(o) ? cJSON_IsTrue(o) : !cJSON_IsFalse(a);
    os_lock(&lock);
    const int was_always = ap_always;
    if (!on) ap_always = 0;
    else if (cJSON_IsTrue(a)) ap_always = 1;
    const int always = ap_always;
    const int was = ap_on;
    if (on) {
        ap_on = 1;
        ap_leave = 0;
        ap_until = os_now() + AP_S;
    } else if (was) {
        ap_leave = 1;
    }
    if (on && !was) {
        unsigned m[6] = {0};
        FILE *f = fopen("/sys/class/net/" IFACE "/address", "r");
        if (f) {
            if (fscanf(f, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) m[4] = m[5] = 0;
            fclose(f);
        }
        snprintf(ap_ssid, sizeof ap_ssid, "DoitDuck-%02X%02X", m[4], m[5]);
    }
    os_unlock(&lock);
    if (always != was_always) store_kv_set("wifi_ap.txt", always ? "1" : "0");
    if (on && !was) os_thread(hotspot, NULL, 0, -1);
    cJSON_AddStringToObject(r, "ssid", ap_ssid);
    cJSON_AddStringToObject(r, "url", "http://" AP_IP "/");
    cJSON_AddBoolToObject(r, "always", always);
    return NULL;
}

/* The hotspot kept open by wifi.ap {always: true} opens again. */
static void start(void)
{
    char v[4];
    if (store_kv_get("wifi_ap.txt", v, sizeof v) <= 0 || v[0] != '1') return;
    cJSON *p = cJSON_Parse("{\"always\":true}"), *r = cJSON_CreateObject();
    wifi_ap(0, p, r);
    cJSON_Delete(p);
    cJSON_Delete(r);
}

static const struct api_method METHODS[] = {
    {"wifi.status", wifi_status, 1},
    {"wifi.scan", wifi_scan, 1},
    {"wifi.connect", wifi_connect, 1},
    {"wifi.ap", wifi_ap, 0}, /* also by voice */
};

const struct feature wifi_feature = {
    .name = "wifi",
    .configure = configure,
    .start = start,
    .methods = METHODS,
    .nmethods = 4,
};
