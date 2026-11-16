#ifndef BLUFI_BLE_H
#define BLUFI_BLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct blufi_ble blufi_ble;

typedef void (*blufi_ble_receive_callback)(const uint8_t *data, size_t len, void *context);
typedef void (*blufi_ble_connection_callback)(bool connected, void *context);

typedef struct {
    blufi_ble_receive_callback receive;
    blufi_ble_connection_callback connection;
    void *context;
} blufi_ble_callbacks;

blufi_ble *blufi_ble_create(const blufi_ble_callbacks *callbacks);
void blufi_ble_destroy(blufi_ble *ble);
int blufi_ble_start(blufi_ble *ble, const char *device_name);
int blufi_ble_send(blufi_ble *ble, const uint8_t *data, size_t len);
int blufi_ble_disconnect(blufi_ble *ble);

#ifdef __cplusplus
}
#endif

#endif
