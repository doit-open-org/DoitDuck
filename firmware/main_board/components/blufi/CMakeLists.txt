# BluFi, Espressif's Wi-Fi setup over BLE (the EspBlufi app): the protocol, its
# security on Mbed TLS, and the BLE side on BlueZ's D-Bus API through GIO.
# From hi_aiot_solution's BluFi sample (Apache-2.0); used by main/features/blufi.
hifw_component_register(
    SRCS blufi_protocol.c blufi_security.c blufi_ble.c
    INCLUDE_DIRS .
    REQUIRES mbedtls
    PKG_CONFIG gio-2.0
    COMPILE_OPTIONS -D_GNU_SOURCE ${DOITDUCK_CFLAGS})
