#ifndef BLUFI_PROTOCOL_H
#define BLUFI_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLUFI_SSID_MAX_LEN 32U
#define BLUFI_PASSWORD_MAX_LEN 64U
#define BLUFI_BSSID_LEN 6U
#define BLUFI_SCAN_RESULT_MAX 32U

typedef enum {
    BLUFI_WIFI_MODE_NONE = 0,
    BLUFI_WIFI_MODE_STA = 1,
    BLUFI_WIFI_MODE_SOFTAP = 2,
    BLUFI_WIFI_MODE_STA_SOFTAP = 3
} blufi_wifi_mode;

typedef enum {
    BLUFI_STA_CONNECTED = 0,
    BLUFI_STA_DISCONNECTED = 1,
    BLUFI_STA_CONNECTING = 2,
    BLUFI_STA_NO_IP = 3
} blufi_sta_state;

typedef enum {
    BLUFI_ERROR_SEQUENCE = 0,
    BLUFI_ERROR_CHECKSUM = 1,
    BLUFI_ERROR_DECRYPT = 2,
    BLUFI_ERROR_ENCRYPT = 3,
    BLUFI_ERROR_SECURITY_INIT = 4,
    BLUFI_ERROR_DH_ALLOC = 5,
    BLUFI_ERROR_DH_PARAM = 6,
    BLUFI_ERROR_DH_READ = 7,
    BLUFI_ERROR_DH_PUBLIC = 8,
    BLUFI_ERROR_DATA_FORMAT = 9,
    BLUFI_ERROR_MD5 = 10,
    BLUFI_ERROR_WIFI_SCAN = 11
} blufi_error;

typedef struct {
    uint8_t ssid[BLUFI_SSID_MAX_LEN];
    size_t ssid_len;
    uint8_t password[BLUFI_PASSWORD_MAX_LEN];
    size_t password_len;
    uint8_t bssid[BLUFI_BSSID_LEN];
    bool bssid_set;
} blufi_sta_config;

typedef struct {
    uint8_t ssid[BLUFI_SSID_MAX_LEN];
    size_t ssid_len;
    int8_t rssi;
} blufi_scan_result;

typedef int (*blufi_transport_send_callback)(const uint8_t *data, size_t len, void *context);
typedef void (*blufi_connect_request_callback)(const blufi_sta_config *config, void *context);
typedef void (*blufi_disconnect_request_callback)(void *context);
typedef void (*blufi_scan_request_callback)(void *context);
typedef void (*blufi_ble_disconnect_request_callback)(void *context);

typedef struct {
    blufi_transport_send_callback send;
    blufi_connect_request_callback connect;
    blufi_disconnect_request_callback disconnect;
    blufi_scan_request_callback scan;
    blufi_ble_disconnect_request_callback disconnect_ble;
    void *context;
} blufi_protocol_callbacks;

typedef struct blufi_protocol blufi_protocol;

blufi_protocol *blufi_protocol_create(const blufi_protocol_callbacks *callbacks);
void blufi_protocol_destroy(blufi_protocol *protocol);
void blufi_protocol_reset(blufi_protocol *protocol);
int blufi_protocol_receive(blufi_protocol *protocol, const uint8_t *frame, size_t frame_len);
int blufi_protocol_report_wifi_status(blufi_protocol *protocol, blufi_sta_state state,
    const uint8_t *ssid, size_t ssid_len, const uint8_t *bssid, size_t bssid_len);
int blufi_protocol_report_scan_results(blufi_protocol *protocol,
    const blufi_scan_result *results, size_t count);
/* Custom data (subtype 0x13) to the phone, which the EspBlufi app shows as it is. */
int blufi_protocol_send_custom(blufi_protocol *protocol, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
