/*
 * The frame constants and wire format implement Espressif's Apache-2.0
 * BluFi protocol specification.
 *
 * From hi_aiot_solution's BluFi sample (Apache-2.0), with Mbed TLS in place of
 * OpenSSL and without the log of every frame sent.
 */

#define _POSIX_C_SOURCE 200809L

#include "blufi_protocol.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "blufi_security.h"
#include "mbedtls/platform_util.h"

#define BLUFI_HEADER_LEN 4U
#define BLUFI_CHECKSUM_LEN 2U
#define BLUFI_FRAGMENT_PREFIX_LEN 2U
#define BLUFI_FRAGMENT_PAYLOAD_MAX 12U
#define BLUFI_FRAGMENT_INTERVAL_MS 10U
#define BLUFI_REASSEMBLY_MAX 1024U
#define BLUFI_PUBLIC_KEY_MAX 128U
#define BLUFI_WIFI_LIST_ENTRY_MAX (BLUFI_SSID_MAX_LEN + 2U)

#define BLUFI_TYPE_MASK 0x03U
#define BLUFI_SUBTYPE_SHIFT 2U
#define BLUFI_TYPE_CTRL 0U
#define BLUFI_TYPE_DATA 1U

#define BLUFI_CTRL_ACK 0U
#define BLUFI_CTRL_SET_SEC_MODE 1U
#define BLUFI_CTRL_SET_WIFI_MODE 2U
#define BLUFI_CTRL_CONNECT 3U
#define BLUFI_CTRL_DISCONNECT 4U
#define BLUFI_CTRL_GET_STATUS 5U
#define BLUFI_CTRL_GET_VERSION 7U
#define BLUFI_CTRL_DISCONNECT_BLE 8U
#define BLUFI_CTRL_GET_WIFI_LIST 9U

#define BLUFI_DATA_NEGOTIATION 0U
#define BLUFI_DATA_STA_BSSID 1U
#define BLUFI_DATA_STA_SSID 2U
#define BLUFI_DATA_STA_PASSWORD 3U
#define BLUFI_DATA_WIFI_STATUS 15U
#define BLUFI_DATA_VERSION 16U
#define BLUFI_DATA_WIFI_LIST 17U
#define BLUFI_DATA_ERROR 18U
#define BLUFI_DATA_CUSTOM 19U

#define BLUFI_FC_ENCRYPTED 0x01U
#define BLUFI_FC_CHECKSUM 0x02U
#define BLUFI_FC_DEVICE_TO_PHONE 0x04U
#define BLUFI_FC_ACK_REQUIRED 0x08U
#define BLUFI_FC_FRAGMENT 0x10U

#define BLUFI_DATA_SEC_CHECKSUM 0x01U
#define BLUFI_DATA_SEC_ENCRYPT 0x02U
#define BLUFI_CTRL_SEC_CHECKSUM 0x10U

#define BLUFI_VERSION_MAJOR 1U
#define BLUFI_VERSION_MINOR 3U

struct blufi_protocol {
    blufi_protocol_callbacks callbacks;
    blufi_security *security;
    blufi_sta_config station;
    uint8_t *aggregate;
    size_t aggregate_len;
    size_t aggregate_offset;
    uint8_t aggregate_type;
    uint8_t send_sequence;
    uint8_t receive_sequence;
    uint8_t security_mode;
    blufi_wifi_mode wifi_mode;
    blufi_sta_state station_state;
};

static uint8_t blufi_build_type(uint8_t type, uint8_t subtype)
{
    return (uint8_t)((type & BLUFI_TYPE_MASK) | (subtype << BLUFI_SUBTYPE_SHIFT));
}

static uint8_t blufi_get_type(uint8_t value)
{
    return value & BLUFI_TYPE_MASK;
}

static uint8_t blufi_get_subtype(uint8_t value)
{
    return value >> BLUFI_SUBTYPE_SHIFT;
}

static void blufi_clear_aggregate(blufi_protocol *protocol)
{
    if (protocol->aggregate != NULL) {
        mbedtls_platform_zeroize(protocol->aggregate, protocol->aggregate_len);
        free(protocol->aggregate);
        protocol->aggregate = NULL;
    }
    protocol->aggregate_len = 0U;
    protocol->aggregate_offset = 0U;
    protocol->aggregate_type = 0U;
}

static void blufi_wait_for_next_fragment(void)
{
    struct timespec interval = {
        .tv_sec = 0,
        .tv_nsec = (long)BLUFI_FRAGMENT_INTERVAL_MS * 1000000L
    };

    while (nanosleep(&interval, &interval) != 0 && errno == EINTR) {
    }
}

static int blufi_send_frame(blufi_protocol *protocol, uint8_t type, const uint8_t *data,
    size_t data_len, bool fragmented, size_t remaining_len)
{
    uint8_t frame[BLUFI_HEADER_LEN + BLUFI_FRAGMENT_PREFIX_LEN +
        BLUFI_FRAGMENT_PAYLOAD_MAX + BLUFI_CHECKSUM_LEN];
    size_t wire_data_len = data_len;
    size_t frame_len;
    size_t data_offset = BLUFI_HEADER_LEN;
    uint8_t frame_control = BLUFI_FC_DEVICE_TO_PHONE;
    uint16_t checksum;
    bool is_control;
    bool is_negotiation;
    bool is_error;

    if (protocol == NULL || protocol->callbacks.send == NULL ||
        (data == NULL && data_len != 0U) || data_len > BLUFI_FRAGMENT_PAYLOAD_MAX ||
        (fragmented && remaining_len > UINT16_MAX)) {
        return -1;
    }

    if (fragmented) {
        frame_control |= BLUFI_FC_FRAGMENT;
        wire_data_len += BLUFI_FRAGMENT_PREFIX_LEN;
    }
    if (wire_data_len > UINT8_MAX) {
        return -1;
    }

    frame[0] = type;
    frame[1] = frame_control;
    frame[2] = protocol->send_sequence++;
    frame[3] = (uint8_t)wire_data_len;
    if (fragmented) {
        frame[data_offset] = (uint8_t)(remaining_len & 0xffU);
        frame[data_offset + 1U] = (uint8_t)((remaining_len >> 8U) & 0xffU);
        data_offset += BLUFI_FRAGMENT_PREFIX_LEN;
    }
    if (data_len != 0U) {
        (void)memcpy(frame + data_offset, data, data_len);
    }

    is_control = blufi_get_type(type) == BLUFI_TYPE_CTRL;
    is_negotiation = !is_control && blufi_get_subtype(type) == BLUFI_DATA_NEGOTIATION;
    is_error = !is_control && blufi_get_subtype(type) == BLUFI_DATA_ERROR;

    if ((is_control && (protocol->security_mode & BLUFI_CTRL_SEC_CHECKSUM) != 0U) ||
        (!is_control && !is_negotiation && !is_error &&
        (protocol->security_mode & BLUFI_DATA_SEC_CHECKSUM) != 0U)) {
        frame[1] |= BLUFI_FC_CHECKSUM;
        checksum = blufi_security_checksum(frame + 2U, wire_data_len + 2U);
        frame[BLUFI_HEADER_LEN + wire_data_len] = (uint8_t)(checksum & 0xffU);
        frame[BLUFI_HEADER_LEN + wire_data_len + 1U] = (uint8_t)(checksum >> 8U);
    }

    if (!is_control && !is_negotiation && !is_error &&
        (protocol->security_mode & BLUFI_DATA_SEC_ENCRYPT) != 0U) {
        if (blufi_security_encrypt(protocol->security, frame[2], frame + BLUFI_HEADER_LEN,
            wire_data_len) != 0) {
            return -1;
        }
        frame[1] |= BLUFI_FC_ENCRYPTED;
    }

    frame_len = BLUFI_HEADER_LEN + wire_data_len;
    if ((frame[1] & BLUFI_FC_CHECKSUM) != 0U) {
        frame_len += BLUFI_CHECKSUM_LEN;
    }
    return protocol->callbacks.send(frame, frame_len, protocol->callbacks.context);
}

static int blufi_send(blufi_protocol *protocol, uint8_t type, const uint8_t *data, size_t data_len)
{
    size_t offset = 0U;
    size_t chunk_len;

    if (data_len == 0U) {
        return blufi_send_frame(protocol, type, NULL, 0U, false, 0U);
    }
    if (data == NULL || data_len > UINT16_MAX) {
        return -1;
    }

    while (data_len - offset > BLUFI_FRAGMENT_PAYLOAD_MAX) {
        chunk_len = BLUFI_FRAGMENT_PAYLOAD_MAX;
        if (blufi_send_frame(protocol, type, data + offset, chunk_len, true,
            data_len - offset) != 0) {
            return -1;
        }
        offset += chunk_len;
        blufi_wait_for_next_fragment();
    }

    return blufi_send_frame(protocol, type, data + offset, data_len - offset, false, 0U);
}

static void blufi_send_error(blufi_protocol *protocol, blufi_error error)
{
    uint8_t value = (uint8_t)error;

    (void)blufi_send(protocol, blufi_build_type(BLUFI_TYPE_DATA, BLUFI_DATA_ERROR), &value, 1U);
}

static int blufi_handle_control(blufi_protocol *protocol, uint8_t subtype,
    const uint8_t *data, size_t len)
{
    uint8_t version[2] = {BLUFI_VERSION_MAJOR, BLUFI_VERSION_MINOR};

    switch (subtype) {
        case BLUFI_CTRL_ACK:
            return len == 1U ? 0 : -1;
        case BLUFI_CTRL_SET_SEC_MODE:
            if (len != 1U ||
                ((data[0] & BLUFI_DATA_SEC_ENCRYPT) != 0U &&
                blufi_security_ready(protocol->security) == 0)) {
                return -1;
            }
            protocol->security_mode = data[0];
            return 0;
        case BLUFI_CTRL_SET_WIFI_MODE:
            if (len != 1U || (data[0] != BLUFI_WIFI_MODE_NONE && data[0] != BLUFI_WIFI_MODE_STA)) {
                return -1;
            }
            protocol->wifi_mode = (blufi_wifi_mode)data[0];
            return 0;
        case BLUFI_CTRL_CONNECT:
            if (len != 0U || protocol->wifi_mode != BLUFI_WIFI_MODE_STA ||
                protocol->station.ssid_len == 0U || protocol->callbacks.connect == NULL) {
                return -1;
            }
            protocol->station_state = BLUFI_STA_CONNECTING;
            protocol->callbacks.connect(&protocol->station, protocol->callbacks.context);
            return 0;
        case BLUFI_CTRL_DISCONNECT:
            if (len != 0U || protocol->callbacks.disconnect == NULL) {
                return -1;
            }
            protocol->callbacks.disconnect(protocol->callbacks.context);
            protocol->station_state = BLUFI_STA_DISCONNECTED;
            return blufi_protocol_report_wifi_status(protocol, protocol->station_state,
                protocol->station.ssid, protocol->station.ssid_len,
                protocol->station.bssid_set ? protocol->station.bssid : NULL,
                protocol->station.bssid_set ? BLUFI_BSSID_LEN : 0U);
        case BLUFI_CTRL_GET_STATUS:
            if (len != 0U) {
                return -1;
            }
            return blufi_protocol_report_wifi_status(protocol, protocol->station_state,
                protocol->station.ssid, protocol->station.ssid_len,
                protocol->station.bssid_set ? protocol->station.bssid : NULL,
                protocol->station.bssid_set ? BLUFI_BSSID_LEN : 0U);
        case BLUFI_CTRL_GET_VERSION:
            if (len != 0U) {
                return -1;
            }
            return blufi_send(protocol, blufi_build_type(BLUFI_TYPE_DATA, BLUFI_DATA_VERSION),
                version, sizeof(version));
        case BLUFI_CTRL_DISCONNECT_BLE:
            if (len != 0U || protocol->callbacks.disconnect_ble == NULL) {
                return -1;
            }
            protocol->callbacks.disconnect_ble(protocol->callbacks.context);
            return 0;
        case BLUFI_CTRL_GET_WIFI_LIST:
            if (len != 0U || protocol->callbacks.scan == NULL) {
                return -1;
            }
            protocol->callbacks.scan(protocol->callbacks.context);
            return 0;
        default:
            return -1;
    }
}

static int blufi_store_value(uint8_t *destination, size_t capacity, size_t *stored_len,
    const uint8_t *data, size_t len)
{
    if (destination == NULL || stored_len == NULL || (data == NULL && len != 0U) || len > capacity) {
        return -1;
    }

    mbedtls_platform_zeroize(destination, capacity);
    if (len != 0U) {
        (void)memcpy(destination, data, len);
    }
    *stored_len = len;
    return 0;
}

static int blufi_handle_data(blufi_protocol *protocol, uint8_t subtype,
    const uint8_t *data, size_t len)
{
    uint8_t response[BLUFI_PUBLIC_KEY_MAX];
    size_t response_len = 0U;
    int ret;

    switch (subtype) {
        case BLUFI_DATA_NEGOTIATION:
            ret = blufi_security_negotiate(protocol->security, data, len,
                response, sizeof(response), &response_len);
            if (ret == 0 && response_len != 0U) {
                ret = blufi_send(protocol,
                    blufi_build_type(BLUFI_TYPE_DATA, BLUFI_DATA_NEGOTIATION),
                    response, response_len);
            }
            mbedtls_platform_zeroize(response, sizeof(response));
            return ret;
        case BLUFI_DATA_STA_BSSID:
            if (len != BLUFI_BSSID_LEN) {
                return -1;
            }
            (void)memcpy(protocol->station.bssid, data, BLUFI_BSSID_LEN);
            protocol->station.bssid_set = true;
            return 0;
        case BLUFI_DATA_STA_SSID:
            return blufi_store_value(protocol->station.ssid, sizeof(protocol->station.ssid),
                &protocol->station.ssid_len, data, len);
        case BLUFI_DATA_STA_PASSWORD:
            return blufi_store_value(protocol->station.password, sizeof(protocol->station.password),
                &protocol->station.password_len, data, len);
        default:
            return -1;
    }
}

static int blufi_dispatch(blufi_protocol *protocol, uint8_t type, const uint8_t *data, size_t len)
{
    if (blufi_get_type(type) == BLUFI_TYPE_CTRL) {
        return blufi_handle_control(protocol, blufi_get_subtype(type), data, len);
    }
    if (blufi_get_type(type) == BLUFI_TYPE_DATA) {
        return blufi_handle_data(protocol, blufi_get_subtype(type), data, len);
    }
    return -1;
}

blufi_protocol *blufi_protocol_create(const blufi_protocol_callbacks *callbacks)
{
    blufi_protocol *protocol;

    if (callbacks == NULL || callbacks->send == NULL) {
        return NULL;
    }

    protocol = calloc(1U, sizeof(*protocol));
    if (protocol == NULL) {
        return NULL;
    }
    protocol->security = blufi_security_create();
    if (protocol->security == NULL) {
        free(protocol);
        return NULL;
    }
    protocol->callbacks = *callbacks;
    protocol->station_state = BLUFI_STA_DISCONNECTED;
    return protocol;
}

void blufi_protocol_reset(blufi_protocol *protocol)
{
    if (protocol == NULL) {
        return;
    }

    blufi_clear_aggregate(protocol);
    blufi_security_reset(protocol->security);
    mbedtls_platform_zeroize(&protocol->station, sizeof(protocol->station));
    protocol->send_sequence = 0U;
    protocol->receive_sequence = 0U;
    protocol->security_mode = 0U;
    protocol->wifi_mode = BLUFI_WIFI_MODE_NONE;
    protocol->station_state = BLUFI_STA_DISCONNECTED;
}

void blufi_protocol_destroy(blufi_protocol *protocol)
{
    if (protocol == NULL) {
        return;
    }

    blufi_protocol_reset(protocol);
    blufi_security_destroy(protocol->security);
    mbedtls_platform_zeroize(protocol, sizeof(*protocol));
    free(protocol);
}

static int blufi_append_fragment(blufi_protocol *protocol, uint8_t type,
    const uint8_t *data, size_t data_len)
{
    size_t total_len;
    size_t payload_len;

    if (data_len < BLUFI_FRAGMENT_PREFIX_LEN) {
        return -1;
    }
    total_len = (size_t)data[0] | ((size_t)data[1] << 8U);
    payload_len = data_len - BLUFI_FRAGMENT_PREFIX_LEN;

    if (protocol->aggregate == NULL) {
        if (total_len == 0U || total_len > BLUFI_REASSEMBLY_MAX || payload_len > total_len) {
            return -1;
        }
        protocol->aggregate = calloc(total_len, 1U);
        if (protocol->aggregate == NULL) {
            return -1;
        }
        protocol->aggregate_len = total_len;
        protocol->aggregate_type = type;
    }

    if (type != protocol->aggregate_type || payload_len > protocol->aggregate_len - protocol->aggregate_offset) {
        blufi_clear_aggregate(protocol);
        return -1;
    }
    (void)memcpy(protocol->aggregate + protocol->aggregate_offset,
        data + BLUFI_FRAGMENT_PREFIX_LEN, payload_len);
    protocol->aggregate_offset += payload_len;
    return 0;
}

static int blufi_finish_packet(blufi_protocol *protocol, uint8_t type,
    const uint8_t *data, size_t data_len)
{
    int ret;

    if (protocol->aggregate == NULL) {
        return blufi_dispatch(protocol, type, data, data_len);
    }
    if (type != protocol->aggregate_type || data_len != protocol->aggregate_len - protocol->aggregate_offset) {
        blufi_clear_aggregate(protocol);
        return -1;
    }

    if (data_len != 0U) {
        (void)memcpy(protocol->aggregate + protocol->aggregate_offset, data, data_len);
    }
    ret = blufi_dispatch(protocol, type, protocol->aggregate, protocol->aggregate_len);
    blufi_clear_aggregate(protocol);
    return ret;
}

int blufi_protocol_receive(blufi_protocol *protocol, const uint8_t *frame, size_t frame_len)
{
    uint8_t *packet;
    uint8_t frame_control;
    uint8_t data_len;
    size_t expected_len;
    uint16_t expected_checksum;
    uint16_t actual_checksum;
    int ret;

    if (protocol == NULL || frame == NULL || frame_len < BLUFI_HEADER_LEN) {
        return -1;
    }

    frame_control = frame[1];
    data_len = frame[3];
    expected_len = BLUFI_HEADER_LEN + (size_t)data_len;
    if ((frame_control & BLUFI_FC_CHECKSUM) != 0U) {
        expected_len += BLUFI_CHECKSUM_LEN;
    }
    if (frame_len != expected_len || (frame_control & BLUFI_FC_DEVICE_TO_PHONE) != 0U) {
        blufi_send_error(protocol, BLUFI_ERROR_DATA_FORMAT);
        return -1;
    }
    if (frame[2] != protocol->receive_sequence) {
        blufi_clear_aggregate(protocol);
        blufi_send_error(protocol, BLUFI_ERROR_SEQUENCE);
        return -1;
    }
    protocol->receive_sequence++;

    packet = malloc(frame_len);
    if (packet == NULL) {
        return -1;
    }
    (void)memcpy(packet, frame, frame_len);

    if ((frame_control & BLUFI_FC_ENCRYPTED) != 0U &&
        blufi_security_decrypt(protocol->security, packet[2], packet + BLUFI_HEADER_LEN,
        data_len) != 0) {
        blufi_clear_aggregate(protocol);
        blufi_send_error(protocol, BLUFI_ERROR_DECRYPT);
        ret = -1;
        goto cleanup;
    }

    if ((frame_control & BLUFI_FC_CHECKSUM) != 0U) {
        expected_checksum = (uint16_t)packet[BLUFI_HEADER_LEN + data_len] |
            ((uint16_t)packet[BLUFI_HEADER_LEN + data_len + 1U] << 8U);
        actual_checksum = blufi_security_checksum(packet + 2U, (size_t)data_len + 2U);
        if (actual_checksum != expected_checksum) {
            blufi_clear_aggregate(protocol);
            blufi_send_error(protocol, BLUFI_ERROR_CHECKSUM);
            ret = -1;
            goto cleanup;
        }
    }

    if ((frame_control & BLUFI_FC_ACK_REQUIRED) != 0U) {
        (void)blufi_send(protocol, blufi_build_type(BLUFI_TYPE_CTRL, BLUFI_CTRL_ACK),
            packet + 2U, 1U);
    }

    if ((frame_control & BLUFI_FC_FRAGMENT) != 0U) {
        ret = blufi_append_fragment(protocol, packet[0], packet + BLUFI_HEADER_LEN, data_len);
    } else {
        ret = blufi_finish_packet(protocol, packet[0], packet + BLUFI_HEADER_LEN, data_len);
    }
    if (ret != 0) {
        blufi_send_error(protocol, BLUFI_ERROR_DATA_FORMAT);
    }

cleanup:
    mbedtls_platform_zeroize(packet, frame_len);
    free(packet);
    return ret;
}

int blufi_protocol_report_wifi_status(blufi_protocol *protocol, blufi_sta_state state,
    const uint8_t *ssid, size_t ssid_len, const uint8_t *bssid, size_t bssid_len)
{
    uint8_t report[3U + 2U + BLUFI_BSSID_LEN + 2U + BLUFI_SSID_MAX_LEN];
    size_t report_len = 3U;

    if (protocol == NULL || (ssid == NULL && ssid_len != 0U) || ssid_len > BLUFI_SSID_MAX_LEN ||
        (bssid == NULL && bssid_len != 0U) || (bssid_len != 0U && bssid_len != BLUFI_BSSID_LEN)) {
        return -1;
    }

    protocol->station_state = state;
    if (bssid_len == BLUFI_BSSID_LEN) {
        (void)memcpy(protocol->station.bssid, bssid, BLUFI_BSSID_LEN);
        protocol->station.bssid_set = true;
    }
    report[0] = (uint8_t)protocol->wifi_mode;
    report[1] = (uint8_t)state;
    report[2] = 0U;
    if (bssid_len == BLUFI_BSSID_LEN) {
        report[report_len++] = BLUFI_DATA_STA_BSSID;
        report[report_len++] = BLUFI_BSSID_LEN;
        (void)memcpy(report + report_len, bssid, BLUFI_BSSID_LEN);
        report_len += BLUFI_BSSID_LEN;
    }
    if (ssid_len != 0U) {
        report[report_len++] = BLUFI_DATA_STA_SSID;
        report[report_len++] = (uint8_t)ssid_len;
        (void)memcpy(report + report_len, ssid, ssid_len);
        report_len += ssid_len;
    }
    return blufi_send(protocol, blufi_build_type(BLUFI_TYPE_DATA, BLUFI_DATA_WIFI_STATUS),
        report, report_len);
}

int blufi_protocol_report_scan_results(blufi_protocol *protocol, const blufi_scan_result *results,
    size_t count)
{
    uint8_t *data;
    size_t capacity;
    size_t offset = 0U;
    size_t i;
    int ret;

    if (protocol == NULL || (results == NULL && count != 0U) || count > BLUFI_SCAN_RESULT_MAX) {
        return -1;
    }
    if (count == 0U) {
        return blufi_send(protocol, blufi_build_type(BLUFI_TYPE_DATA, BLUFI_DATA_WIFI_LIST),
            NULL, 0U);
    }

    capacity = count * BLUFI_WIFI_LIST_ENTRY_MAX;
    data = calloc(capacity, 1U);
    if (data == NULL) {
        return -1;
    }

    for (i = 0U; i < count; i++) {
        if (results[i].ssid_len > BLUFI_SSID_MAX_LEN ||
            results[i].ssid_len + 2U > capacity - offset) {
            ret = -1;
            goto cleanup;
        }
        data[offset++] = (uint8_t)(results[i].ssid_len + 1U);
        data[offset++] = (uint8_t)results[i].rssi;
        (void)memcpy(data + offset, results[i].ssid, results[i].ssid_len);
        offset += results[i].ssid_len;
    }

    ret = blufi_send(protocol, blufi_build_type(BLUFI_TYPE_DATA, BLUFI_DATA_WIFI_LIST),
        data, offset);

cleanup:
    mbedtls_platform_zeroize(data, capacity);
    free(data);
    return ret;
}

int blufi_protocol_send_custom(blufi_protocol *protocol, const uint8_t *data, size_t len)
{
    if (protocol == NULL || data == NULL || len == 0U) {
        return -1;
    }
    return blufi_send(protocol, blufi_build_type(BLUFI_TYPE_DATA, BLUFI_DATA_CUSTOM), data, len);
}
