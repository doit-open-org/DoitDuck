/*
 * BluFi's BLE side over BlueZ's D-Bus API: a GATT service 0xFFFF with the write
 * characteristic 0xFF01 and the notify characteristic 0xFF02, and an advertisement
 * by name, from a GLib main loop on a thread of its own.
 *
 * From hi_aiot_solution's BluFi sample (Apache-2.0); D-Bus and bluetoothd are
 * started by the image's ble-service.
 */
#include "blufi_ble.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>

#define BLUEZ_BUS_NAME "org.bluez"
#define DBUS_PROPERTIES_IFACE "org.freedesktop.DBus.Properties"
#define DBUS_OBJECT_MANAGER_IFACE "org.freedesktop.DBus.ObjectManager"
#define BLUEZ_ADAPTER_IFACE "org.bluez.Adapter1"
#define BLUEZ_DEVICE_IFACE "org.bluez.Device1"
#define BLUEZ_GATT_MANAGER_IFACE "org.bluez.GattManager1"
#define BLUEZ_GATT_SERVICE_IFACE "org.bluez.GattService1"
#define BLUEZ_GATT_CHARACTERISTIC_IFACE "org.bluez.GattCharacteristic1"
#define BLUEZ_ADV_MANAGER_IFACE "org.bluez.LEAdvertisingManager1"
#define BLUEZ_ADVERTISEMENT_IFACE "org.bluez.LEAdvertisement1"

#define BLUFI_APP_PATH "/com/hisilicon/blufi"
#define BLUFI_SERVICE_PATH BLUFI_APP_PATH "/service0"
#define BLUFI_WRITE_PATH BLUFI_SERVICE_PATH "/char0"
#define BLUFI_NOTIFY_PATH BLUFI_SERVICE_PATH "/char1"
#define BLUFI_ADVERTISEMENT_PATH BLUFI_APP_PATH "/advertisement0"

#define BLUFI_SERVICE_UUID "0000ffff-0000-1000-8000-00805f9b34fb"
#define BLUFI_WRITE_UUID "0000ff01-0000-1000-8000-00805f9b34fb"
#define BLUFI_NOTIFY_UUID "0000ff02-0000-1000-8000-00805f9b34fb"
#define BLUFI_BLE_WRITE_MAX 512U
#define BLUFI_BLE_NAME_MAX 29U
#define BLUFI_BLE_START_POLLS 20U
#define BLUFI_BLE_START_POLL_MS 250U

typedef struct {
    blufi_ble *ble;
    GBytes *value;
} blufi_ble_send_context;

struct blufi_ble {
    pthread_mutex_t lock;
    pthread_cond_t ready_condition;
    pthread_t thread;
    blufi_ble_callbacks callbacks;
    GMainLoop *loop;
    GMainContext *main_context;
    GDBusConnection *connection;
    GDBusNodeInfo *object_manager_info;
    GDBusNodeInfo *service_info;
    GDBusNodeInfo *characteristic_info;
    GDBusNodeInfo *advertisement_info;
    GBytes *notify_value;
    char *adapter_path;
    char *connected_device_path;
    char device_name[BLUFI_BLE_NAME_MAX + 1U];
    guint registrations[5];
    guint properties_subscription;
    bool thread_started;
    bool ready;
    bool failed;
    bool notifying;
    bool connected;
    bool application_registered;
    bool advertisement_registered;
};

static const char g_object_manager_xml[] =
    "<node>"
    " <interface name='org.freedesktop.DBus.ObjectManager'>"
    "  <method name='GetManagedObjects'>"
    "   <arg name='objects' type='a{oa{sa{sv}}}' direction='out'/>"
    "  </method>"
    " </interface>"
    "</node>";

static const char g_service_xml[] =
    "<node>"
    " <interface name='org.bluez.GattService1'>"
    "  <property name='UUID' type='s' access='read'/>"
    "  <property name='Primary' type='b' access='read'/>"
    " </interface>"
    "</node>";

static const char g_characteristic_xml[] =
    "<node>"
    " <interface name='org.bluez.GattCharacteristic1'>"
    "  <method name='ReadValue'>"
    "   <arg name='options' type='a{sv}' direction='in'/>"
    "   <arg name='value' type='ay' direction='out'/>"
    "  </method>"
    "  <method name='WriteValue'>"
    "   <arg name='value' type='ay' direction='in'/>"
    "   <arg name='options' type='a{sv}' direction='in'/>"
    "  </method>"
    "  <method name='StartNotify'/>"
    "  <method name='StopNotify'/>"
    "  <property name='UUID' type='s' access='read'/>"
    "  <property name='Service' type='o' access='read'/>"
    "  <property name='Flags' type='as' access='read'/>"
    "  <property name='Value' type='ay' access='read'/>"
    "  <property name='Notifying' type='b' access='read'/>"
    " </interface>"
    "</node>";

static const char g_advertisement_xml[] =
    "<node>"
    " <interface name='org.bluez.LEAdvertisement1'>"
    "  <method name='Release'/>"
    "  <property name='Type' type='s' access='read'/>"
    "  <property name='ServiceUUIDs' type='as' access='read'/>"
    "  <property name='LocalName' type='s' access='read'/>"
    "  <property name='Discoverable' type='b' access='read'/>"
    " </interface>"
    "</node>";

static int blufi_ble_run_command(const char *command)
{
    int status;

    if (command == NULL) {
        return -1;
    }
    status = system(command);
    if (status == -1 || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status) == 0 ? 0 : -1;
}

static void blufi_ble_sleep_ms(unsigned int milliseconds)
{
    struct timespec duration = {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L
    };

    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) {
    }
}

/* The image's ble-service starts the system D-Bus and bluetoothd; S51wireless brings up hci0. */
static int blufi_ble_prepare_runtime(void)
{
    if (blufi_ble_run_command("test -d /sys/class/bluetooth/hci0") != 0) {
        fprintf(stderr, "blufi: hci0 is unavailable; check S51wireless\n");
        return -1;
    }
    if (blufi_ble_run_command("ble-service start >/dev/null") != 0) {
        fprintf(stderr, "blufi: ble-service start failed; see /run/ble-service-bluetoothd.log\n");
        return -1;
    }
    return 0;
}

static GVariant *blufi_ble_byte_array_variant(GBytes *bytes)
{
    gconstpointer data;
    gsize len;

    if (bytes == NULL) {
        return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, NULL, 0U, sizeof(guint8));
    }
    data = g_bytes_get_data(bytes, &len);
    return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data, len, sizeof(guint8));
}

static GVariant *blufi_ble_flags_variant(bool notify_characteristic)
{
    GVariantBuilder builder;

    g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
    if (notify_characteristic) {
        g_variant_builder_add(&builder, "s", "read");
        g_variant_builder_add(&builder, "s", "notify");
    } else {
        g_variant_builder_add(&builder, "s", "write");
        g_variant_builder_add(&builder, "s", "write-without-response");
    }
    return g_variant_builder_end(&builder);
}

static void blufi_ble_add_managed_service(GVariantBuilder *objects)
{
    GVariantBuilder interfaces;
    GVariantBuilder properties;

    g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&properties, "{sv}", "UUID", g_variant_new_string(BLUFI_SERVICE_UUID));
    g_variant_builder_add(&properties, "{sv}", "Primary", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&interfaces, "{s@a{sv}}", BLUEZ_GATT_SERVICE_IFACE,
        g_variant_builder_end(&properties));
    g_variant_builder_add(objects, "{o@a{sa{sv}}}", BLUFI_SERVICE_PATH,
        g_variant_builder_end(&interfaces));
}

static void blufi_ble_add_managed_characteristic(GVariantBuilder *objects, const char *path,
    const char *uuid, bool notify_characteristic, GBytes *value, bool notifying)
{
    GVariantBuilder interfaces;
    GVariantBuilder properties;

    g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
    g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&properties, "{sv}", "UUID", g_variant_new_string(uuid));
    g_variant_builder_add(&properties, "{sv}", "Service", g_variant_new_object_path(BLUFI_SERVICE_PATH));
    g_variant_builder_add(&properties, "{sv}", "Flags", blufi_ble_flags_variant(notify_characteristic));
    if (notify_characteristic) {
        g_variant_builder_add(&properties, "{sv}", "Value", blufi_ble_byte_array_variant(value));
        g_variant_builder_add(&properties, "{sv}", "Notifying", g_variant_new_boolean(notifying));
    }
    g_variant_builder_add(&interfaces, "{s@a{sv}}", BLUEZ_GATT_CHARACTERISTIC_IFACE,
        g_variant_builder_end(&properties));
    g_variant_builder_add(objects, "{o@a{sa{sv}}}", path, g_variant_builder_end(&interfaces));
}

static void blufi_ble_object_manager_method(GDBusConnection *connection, const char *sender,
    const char *object_path, const char *interface_name, const char *method_name,
    GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data)
{
    blufi_ble *ble = user_data;
    GVariantBuilder objects;

    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)parameters;
    if (strcmp(method_name, "GetManagedObjects") != 0) {
        g_dbus_method_invocation_return_dbus_error(invocation,
            "org.bluez.Error.NotSupported", "Unsupported method");
        return;
    }

    g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
    blufi_ble_add_managed_service(&objects);
    blufi_ble_add_managed_characteristic(&objects, BLUFI_WRITE_PATH, BLUFI_WRITE_UUID,
        false, NULL, false);
    blufi_ble_add_managed_characteristic(&objects, BLUFI_NOTIFY_PATH, BLUFI_NOTIFY_UUID,
        true, ble->notify_value, ble->notifying);
    g_dbus_method_invocation_return_value(invocation,
        g_variant_new("(@a{oa{sa{sv}}})", g_variant_builder_end(&objects)));
}

static GVariant *blufi_ble_service_property(GDBusConnection *connection, const char *sender,
    const char *object_path, const char *interface_name, const char *property_name,
    GError **error, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)error;
    (void)user_data;
    if (strcmp(property_name, "UUID") == 0) {
        return g_variant_new_string(BLUFI_SERVICE_UUID);
    }
    if (strcmp(property_name, "Primary") == 0) {
        return g_variant_new_boolean(TRUE);
    }
    return NULL;
}

static void blufi_ble_emit_characteristic_property(blufi_ble *ble, const char *property_name,
    GVariant *value)
{
    GVariantBuilder changed;
    GVariantBuilder invalidated;

    g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&changed, "{sv}", property_name, value);
    g_variant_builder_init(&invalidated, G_VARIANT_TYPE("as"));
    (void)g_dbus_connection_emit_signal(ble->connection, NULL, BLUFI_NOTIFY_PATH,
        DBUS_PROPERTIES_IFACE, "PropertiesChanged",
        g_variant_new("(s@a{sv}@as)", BLUEZ_GATT_CHARACTERISTIC_IFACE,
        g_variant_builder_end(&changed), g_variant_builder_end(&invalidated)), NULL);
}

static void blufi_ble_update_connection(blufi_ble *ble, bool connected,
    const char *device_path)
{
    bool changed = connected != ble->connected;

    if (connected && device_path != NULL) {
        g_free(ble->connected_device_path);
        ble->connected_device_path = g_strdup(device_path);
    }
    ble->connected = connected;
    if (!connected) {
        ble->notifying = false;
        g_clear_pointer(&ble->connected_device_path, g_free);
    }
    if (changed && ble->callbacks.connection != NULL) {
        ble->callbacks.connection(connected, ble->callbacks.context);
    }
}

static void blufi_ble_characteristic_method(GDBusConnection *connection, const char *sender,
    const char *object_path, const char *interface_name, const char *method_name,
    GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data)
{
    blufi_ble *ble = user_data;

    (void)connection;
    (void)sender;
    (void)interface_name;
    if (strcmp(method_name, "WriteValue") == 0 && strcmp(object_path, BLUFI_WRITE_PATH) == 0) {
        GVariant *value;
        GVariant *options;
        GVariant *device_value;
        gconstpointer data;
        gsize len;

        g_variant_get(parameters, "(@ay@a{sv})", &value, &options);
        device_value = g_variant_lookup_value(options, "device", G_VARIANT_TYPE_OBJECT_PATH);
        blufi_ble_update_connection(ble, true,
            device_value != NULL ? g_variant_get_string(device_value, NULL) : NULL);
        g_clear_pointer(&device_value, g_variant_unref);
        data = g_variant_get_fixed_array(value, &len, sizeof(guint8));
        if (data == NULL || len == 0U || len > BLUFI_BLE_WRITE_MAX) {
            g_dbus_method_invocation_return_dbus_error(invocation,
                "org.bluez.Error.InvalidValueLength", "Invalid BluFi frame length");
        } else {
            if (ble->callbacks.receive != NULL) {
                ble->callbacks.receive(data, len, ble->callbacks.context);
            }
            g_dbus_method_invocation_return_value(invocation, NULL);
        }
        g_variant_unref(options);
        g_variant_unref(value);
        return;
    }
    if (strcmp(method_name, "ReadValue") == 0 && strcmp(object_path, BLUFI_NOTIFY_PATH) == 0) {
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(@ay)", blufi_ble_byte_array_variant(ble->notify_value)));
        return;
    }
    if (strcmp(method_name, "StartNotify") == 0 && strcmp(object_path, BLUFI_NOTIFY_PATH) == 0) {
        blufi_ble_update_connection(ble, true, NULL);
        ble->notifying = true;
        blufi_ble_emit_characteristic_property(ble, "Notifying", g_variant_new_boolean(TRUE));
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (strcmp(method_name, "StopNotify") == 0 && strcmp(object_path, BLUFI_NOTIFY_PATH) == 0) {
        blufi_ble_update_connection(ble, false, NULL);
        blufi_ble_emit_characteristic_property(ble, "Notifying", g_variant_new_boolean(FALSE));
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    g_dbus_method_invocation_return_dbus_error(invocation,
        "org.bluez.Error.NotSupported", "Unsupported characteristic operation");
}

static GVariant *blufi_ble_characteristic_property(GDBusConnection *connection, const char *sender,
    const char *object_path, const char *interface_name, const char *property_name,
    GError **error, gpointer user_data)
{
    blufi_ble *ble = user_data;
    bool notify_characteristic = strcmp(object_path, BLUFI_NOTIFY_PATH) == 0;

    (void)connection;
    (void)sender;
    (void)interface_name;
    (void)error;
    if (strcmp(property_name, "UUID") == 0) {
        return g_variant_new_string(notify_characteristic ? BLUFI_NOTIFY_UUID : BLUFI_WRITE_UUID);
    }
    if (strcmp(property_name, "Service") == 0) {
        return g_variant_new_object_path(BLUFI_SERVICE_PATH);
    }
    if (strcmp(property_name, "Flags") == 0) {
        return blufi_ble_flags_variant(notify_characteristic);
    }
    if (notify_characteristic && strcmp(property_name, "Value") == 0) {
        return blufi_ble_byte_array_variant(ble->notify_value);
    }
    if (notify_characteristic && strcmp(property_name, "Notifying") == 0) {
        return g_variant_new_boolean(ble->notifying);
    }
    return NULL;
}

static void blufi_ble_advertisement_method(GDBusConnection *connection, const char *sender,
    const char *object_path, const char *interface_name, const char *method_name,
    GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data)
{
    blufi_ble *ble = user_data;

    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)parameters;
    if (strcmp(method_name, "Release") == 0) {
        ble->advertisement_registered = false;
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    g_dbus_method_invocation_return_dbus_error(invocation,
        "org.bluez.Error.NotSupported", "Unsupported advertisement operation");
}

static GVariant *blufi_ble_advertisement_property(GDBusConnection *connection, const char *sender,
    const char *object_path, const char *interface_name, const char *property_name,
    GError **error, gpointer user_data)
{
    blufi_ble *ble = user_data;
    const char *service_uuids[] = {BLUFI_SERVICE_UUID, NULL};

    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)error;
    if (strcmp(property_name, "Type") == 0) {
        return g_variant_new_string("peripheral");
    }
    if (strcmp(property_name, "ServiceUUIDs") == 0) {
        return g_variant_new_strv(service_uuids, -1);
    }
    if (strcmp(property_name, "LocalName") == 0) {
        return g_variant_new_string(ble->device_name);
    }
    if (strcmp(property_name, "Discoverable") == 0) {
        return g_variant_new_boolean(TRUE);
    }
    return NULL;
}

static const GDBusInterfaceVTable g_object_manager_vtable = {
    .method_call = blufi_ble_object_manager_method
};

static const GDBusInterfaceVTable g_service_vtable = {
    .get_property = blufi_ble_service_property
};

static const GDBusInterfaceVTable g_characteristic_vtable = {
    .method_call = blufi_ble_characteristic_method,
    .get_property = blufi_ble_characteristic_property
};

static const GDBusInterfaceVTable g_advertisement_vtable = {
    .method_call = blufi_ble_advertisement_method,
    .get_property = blufi_ble_advertisement_property
};

static int blufi_ble_register_object(blufi_ble *ble, const char *path,
    GDBusInterfaceInfo *interface_info, const GDBusInterfaceVTable *vtable, guint *registration)
{
    GError *error = NULL;

    *registration = g_dbus_connection_register_object(ble->connection, path,
        interface_info, vtable, ble, NULL, &error);
    if (*registration == 0U) {
        fprintf(stderr, "blufi: register %s failed: %s\n", path,
            error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        return -1;
    }
    return 0;
}

static int blufi_ble_register_local_objects(blufi_ble *ble)
{
    GError *error = NULL;

    ble->object_manager_info = g_dbus_node_info_new_for_xml(g_object_manager_xml, &error);
    if (ble->object_manager_info == NULL) {
        goto error;
    }
    ble->service_info = g_dbus_node_info_new_for_xml(g_service_xml, &error);
    if (ble->service_info == NULL) {
        goto error;
    }
    ble->characteristic_info = g_dbus_node_info_new_for_xml(g_characteristic_xml, &error);
    if (ble->characteristic_info == NULL) {
        goto error;
    }
    ble->advertisement_info = g_dbus_node_info_new_for_xml(g_advertisement_xml, &error);
    if (ble->advertisement_info == NULL) {
        goto error;
    }

    if (blufi_ble_register_object(ble, BLUFI_APP_PATH, ble->object_manager_info->interfaces[0],
        &g_object_manager_vtable, &ble->registrations[0]) != 0 ||
        blufi_ble_register_object(ble, BLUFI_SERVICE_PATH, ble->service_info->interfaces[0],
        &g_service_vtable, &ble->registrations[1]) != 0 ||
        blufi_ble_register_object(ble, BLUFI_WRITE_PATH, ble->characteristic_info->interfaces[0],
        &g_characteristic_vtable, &ble->registrations[2]) != 0 ||
        blufi_ble_register_object(ble, BLUFI_NOTIFY_PATH, ble->characteristic_info->interfaces[0],
        &g_characteristic_vtable, &ble->registrations[3]) != 0 ||
        blufi_ble_register_object(ble, BLUFI_ADVERTISEMENT_PATH,
        ble->advertisement_info->interfaces[0], &g_advertisement_vtable,
        &ble->registrations[4]) != 0) {
        return -1;
    }
    return 0;

error:
    fprintf(stderr, "blufi: parse D-Bus XML failed: %s\n",
        error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
    return -1;
}

static bool blufi_ble_has_interface(GVariant *interfaces, const char *interface_name)
{
    GVariant *properties = g_variant_lookup_value(interfaces, interface_name,
        G_VARIANT_TYPE("a{sv}"));

    if (properties == NULL) {
        return false;
    }
    g_variant_unref(properties);
    return true;
}

static int blufi_ble_find_adapter(blufi_ble *ble, bool require_managers)
{
    GVariant *result;
    GVariantIter *objects;
    const char *path;
    GVariant *interfaces;
    GError *error = NULL;
    bool found = false;

    result = g_dbus_connection_call_sync(ble->connection, BLUEZ_BUS_NAME, "/",
        DBUS_OBJECT_MANAGER_IFACE, "GetManagedObjects", NULL,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
    if (result == NULL) {
        g_clear_error(&error);
        return -1;
    }

    g_variant_get(result, "(a{oa{sa{sv}}})", &objects);
    while (g_variant_iter_next(objects, "{&o@a{sa{sv}}}", &path, &interfaces)) {
        if (blufi_ble_has_interface(interfaces, BLUEZ_ADAPTER_IFACE) &&
            (ble->adapter_path == NULL || strcmp(ble->adapter_path, path) == 0) &&
            (!require_managers ||
            (blufi_ble_has_interface(interfaces, BLUEZ_GATT_MANAGER_IFACE) &&
            blufi_ble_has_interface(interfaces, BLUEZ_ADV_MANAGER_IFACE)))) {
            if (ble->adapter_path == NULL) {
                ble->adapter_path = g_strdup(path);
            }
            found = ble->adapter_path != NULL;
            g_variant_unref(interfaces);
            break;
        }
        g_variant_unref(interfaces);
    }
    g_variant_iter_free(objects);
    g_variant_unref(result);
    return found ? 0 : -1;
}

static int blufi_ble_wait_for_adapter(blufi_ble *ble, bool require_managers)
{
    unsigned int poll;

    for (poll = 0U; poll < BLUFI_BLE_START_POLLS; poll++) {
        if (blufi_ble_find_adapter(ble, require_managers) == 0) {
            return 0;
        }
        blufi_ble_sleep_ms(500U);
    }
    return -1;
}

static GVariant *blufi_ble_manager_parameters(const char *method_name, const char *object_path)
{
    GVariantBuilder options;

    if (strncmp(method_name, "Unregister", 10U) == 0) {
        return g_variant_new("(o)", object_path);
    }
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    return g_variant_new("(o@a{sv})", object_path, g_variant_builder_end(&options));
}

static int blufi_ble_power_adapter(blufi_ble *ble)
{
    GVariant *reply;
    GError *error = NULL;

    reply = g_dbus_connection_call_sync(ble->connection, BLUEZ_BUS_NAME,
        ble->adapter_path, DBUS_PROPERTIES_IFACE, "Set",
        g_variant_new("(ssv)", BLUEZ_ADAPTER_IFACE, "Powered", g_variant_new_boolean(TRUE)),
        NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    if (reply == NULL) {
        fprintf(stderr, "blufi: power adapter failed: %s\n",
            error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        return -1;
    }
    g_variant_unref(reply);
    return 0;
}

static void blufi_ble_properties_changed(GDBusConnection *connection, const char *sender,
    const char *object_path, const char *interface_name, const char *signal_name,
    GVariant *parameters, gpointer user_data)
{
    blufi_ble *ble = user_data;
    const char *changed_interface;
    GVariant *changed;
    GVariant *invalidated;
    GVariant *connected_value;
    gboolean connected;

    (void)connection;
    (void)sender;
    (void)interface_name;
    (void)signal_name;
    g_variant_get(parameters, "(&s@a{sv}@as)", &changed_interface, &changed, &invalidated);
    g_variant_unref(invalidated);
    if (strcmp(changed_interface, BLUEZ_DEVICE_IFACE) != 0) {
        g_variant_unref(changed);
        return;
    }
    connected_value = g_variant_lookup_value(changed, "Connected", G_VARIANT_TYPE_BOOLEAN);
    g_variant_unref(changed);
    if (connected_value == NULL) {
        return;
    }
    connected = g_variant_get_boolean(connected_value);
    g_variant_unref(connected_value);

    if (connected || g_strcmp0(object_path, ble->connected_device_path) == 0) {
        blufi_ble_update_connection(ble, connected, connected ? object_path : NULL);
    }
}

static void blufi_ble_set_start_result(blufi_ble *ble, bool ready)
{
    (void)pthread_mutex_lock(&ble->lock);
    ble->ready = ready;
    ble->failed = !ready;
    (void)pthread_cond_broadcast(&ble->ready_condition);
    (void)pthread_mutex_unlock(&ble->lock);
}

static void blufi_ble_registration_failed(blufi_ble *ble, const char *operation, GError *error)
{
    fprintf(stderr, "blufi: %s failed: %s\n", operation,
        error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
    blufi_ble_set_start_result(ble, false);
    if (ble->loop != NULL) {
        g_main_loop_quit(ble->loop);
    }
}

static void blufi_ble_advertisement_registered(GObject *source_object, GAsyncResult *result,
    gpointer user_data)
{
    blufi_ble *ble = user_data;
    GVariant *reply;
    GError *error = NULL;

    reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source_object), result, &error);
    if (reply == NULL) {
        blufi_ble_registration_failed(ble, "RegisterAdvertisement", error);
        return;
    }
    g_variant_unref(reply);
    ble->advertisement_registered = true;
    blufi_ble_set_start_result(ble, true);
}

static void blufi_ble_application_registered(GObject *source_object, GAsyncResult *result,
    gpointer user_data)
{
    blufi_ble *ble = user_data;
    GVariant *reply;
    GError *error = NULL;

    reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source_object), result, &error);
    if (reply == NULL) {
        blufi_ble_registration_failed(ble, "RegisterApplication", error);
        return;
    }
    g_variant_unref(reply);
    ble->application_registered = true;
    g_dbus_connection_call(ble->connection, BLUEZ_BUS_NAME, ble->adapter_path,
        BLUEZ_ADV_MANAGER_IFACE, "RegisterAdvertisement",
        blufi_ble_manager_parameters("RegisterAdvertisement", BLUFI_ADVERTISEMENT_PATH),
        NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
        blufi_ble_advertisement_registered, ble);
}

static void blufi_ble_begin_registration(blufi_ble *ble)
{
    g_dbus_connection_call(ble->connection, BLUEZ_BUS_NAME, ble->adapter_path,
        BLUEZ_GATT_MANAGER_IFACE, "RegisterApplication",
        blufi_ble_manager_parameters("RegisterApplication", BLUFI_APP_PATH),
        NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
        blufi_ble_application_registered, ble);
}

static void blufi_ble_unregister_objects(blufi_ble *ble)
{
    size_t i;

    if (ble->connection != NULL) {
        for (i = 0U; i < sizeof(ble->registrations) / sizeof(ble->registrations[0]); i++) {
            if (ble->registrations[i] != 0U) {
                (void)g_dbus_connection_unregister_object(ble->connection,
                    ble->registrations[i]);
                ble->registrations[i] = 0U;
            }
        }
    }
}

static void blufi_ble_cleanup(blufi_ble *ble)
{
    if (ble->advertisement_registered) {
        g_dbus_connection_call(ble->connection, BLUEZ_BUS_NAME, ble->adapter_path,
            BLUEZ_ADV_MANAGER_IFACE, "UnregisterAdvertisement",
            blufi_ble_manager_parameters("UnregisterAdvertisement", BLUFI_ADVERTISEMENT_PATH),
            NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL, NULL);
        ble->advertisement_registered = false;
    }
    if (ble->application_registered) {
        g_dbus_connection_call(ble->connection, BLUEZ_BUS_NAME, ble->adapter_path,
            BLUEZ_GATT_MANAGER_IFACE, "UnregisterApplication",
            blufi_ble_manager_parameters("UnregisterApplication", BLUFI_APP_PATH),
            NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL, NULL);
        ble->application_registered = false;
    }
    if (ble->properties_subscription != 0U && ble->connection != NULL) {
        g_dbus_connection_signal_unsubscribe(ble->connection, ble->properties_subscription);
        ble->properties_subscription = 0U;
    }
    blufi_ble_unregister_objects(ble);
    g_clear_pointer(&ble->notify_value, g_bytes_unref);
    g_clear_pointer(&ble->object_manager_info, g_dbus_node_info_unref);
    g_clear_pointer(&ble->service_info, g_dbus_node_info_unref);
    g_clear_pointer(&ble->characteristic_info, g_dbus_node_info_unref);
    g_clear_pointer(&ble->advertisement_info, g_dbus_node_info_unref);
    g_clear_object(&ble->connection);
    g_clear_pointer(&ble->adapter_path, g_free);
    g_clear_pointer(&ble->connected_device_path, g_free);
    g_clear_pointer(&ble->loop, g_main_loop_unref);
    g_clear_pointer(&ble->main_context, g_main_context_unref);
}

static void *blufi_ble_thread(void *argument)
{
    blufi_ble *ble = argument;
    GError *error = NULL;

    if (blufi_ble_prepare_runtime() != 0) {
        blufi_ble_set_start_result(ble, false);
        return NULL;
    }
    ble->connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (ble->connection == NULL) {
        fprintf(stderr, "blufi: connect system bus failed: %s\n",
            error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        blufi_ble_set_start_result(ble, false);
        return NULL;
    }
    ble->main_context = g_main_context_new();
    ble->loop = g_main_loop_new(ble->main_context, FALSE);
    g_main_context_push_thread_default(ble->main_context);

    if (blufi_ble_wait_for_adapter(ble, false) != 0) {
        fprintf(stderr, "blufi: BlueZ adapter not found\n");
        goto start_failed;
    }
    if (blufi_ble_power_adapter(ble) != 0) {
        goto start_failed;
    }
    if (blufi_ble_wait_for_adapter(ble, true) != 0) {
        fprintf(stderr, "blufi: BlueZ GATT or advertising manager not available\n");
        goto start_failed;
    }
    if (blufi_ble_register_local_objects(ble) != 0) {
        goto start_failed;
    }
    ble->properties_subscription = g_dbus_connection_signal_subscribe(ble->connection,
        BLUEZ_BUS_NAME, DBUS_PROPERTIES_IFACE, "PropertiesChanged", NULL,
        BLUEZ_DEVICE_IFACE, G_DBUS_SIGNAL_FLAGS_NONE,
        blufi_ble_properties_changed, ble, NULL);

    blufi_ble_begin_registration(ble);
    g_main_loop_run(ble->loop);
    g_main_context_pop_thread_default(ble->main_context);
    blufi_ble_cleanup(ble);
    return NULL;

start_failed:
    g_main_context_pop_thread_default(ble->main_context);
    blufi_ble_set_start_result(ble, false);
    blufi_ble_cleanup(ble);
    return NULL;
}

static gboolean blufi_ble_send_in_loop(gpointer user_data)
{
    blufi_ble_send_context *send_context = user_data;
    blufi_ble *ble = send_context->ble;

    g_clear_pointer(&ble->notify_value, g_bytes_unref);
    ble->notify_value = g_bytes_ref(send_context->value);
    if (ble->notifying && ble->connection != NULL) {
        blufi_ble_emit_characteristic_property(ble, "Value",
            blufi_ble_byte_array_variant(ble->notify_value));
    }
    g_bytes_unref(send_context->value);
    free(send_context);
    return G_SOURCE_REMOVE;
}

static gboolean blufi_ble_disconnect_in_loop(gpointer user_data)
{
    blufi_ble *ble = user_data;

    if (ble->connected_device_path != NULL) {
        g_dbus_connection_call(ble->connection, BLUEZ_BUS_NAME,
            ble->connected_device_path, BLUEZ_DEVICE_IFACE, "Disconnect", NULL,
            NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL, NULL);
    }
    return G_SOURCE_REMOVE;
}

static gboolean blufi_ble_quit_in_loop(gpointer user_data)
{
    blufi_ble *ble = user_data;

    if (ble->loop != NULL) {
        g_main_loop_quit(ble->loop);
    }
    return G_SOURCE_REMOVE;
}

blufi_ble *blufi_ble_create(const blufi_ble_callbacks *callbacks)
{
    blufi_ble *ble;

    if (callbacks == NULL || callbacks->receive == NULL) {
        return NULL;
    }
    ble = calloc(1U, sizeof(*ble));
    if (ble == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&ble->lock, NULL) != 0) {
        free(ble);
        return NULL;
    }
    if (pthread_cond_init(&ble->ready_condition, NULL) != 0) {
        (void)pthread_mutex_destroy(&ble->lock);
        free(ble);
        return NULL;
    }
    ble->callbacks = *callbacks;
    return ble;
}

int blufi_ble_start(blufi_ble *ble, const char *device_name)
{
    size_t name_len;
    int ret;

    if (ble == NULL || device_name == NULL) {
        return -1;
    }
    name_len = strnlen(device_name, BLUFI_BLE_NAME_MAX + 1U);
    if (name_len == 0U || name_len > BLUFI_BLE_NAME_MAX) {
        return -1;
    }
    (void)memcpy(ble->device_name, device_name, name_len);
    ble->device_name[name_len] = '\0';

    (void)pthread_mutex_lock(&ble->lock);
    if (ble->thread_started) {
        ret = ble->ready ? 0 : -1;
        (void)pthread_mutex_unlock(&ble->lock);
        return ret;
    }
    ble->thread_started = true;
    ret = pthread_create(&ble->thread, NULL, blufi_ble_thread, ble);
    if (ret != 0) {
        ble->thread_started = false;
        (void)pthread_mutex_unlock(&ble->lock);
        return -1;
    }
    while (!ble->ready && !ble->failed) {
        (void)pthread_cond_wait(&ble->ready_condition, &ble->lock);
    }
    ret = ble->ready ? 0 : -1;
    (void)pthread_mutex_unlock(&ble->lock);
    if (ret != 0) {
        (void)pthread_join(ble->thread, NULL);
        (void)pthread_mutex_lock(&ble->lock);
        ble->thread_started = false;
        (void)pthread_mutex_unlock(&ble->lock);
    }
    return ret;
}

int blufi_ble_send(blufi_ble *ble, const uint8_t *data, size_t len)
{
    blufi_ble_send_context *send_context;

    if (ble == NULL || data == NULL || len == 0U || len > BLUFI_BLE_WRITE_MAX ||
        ble->main_context == NULL) {
        return -1;
    }
    send_context = calloc(1U, sizeof(*send_context));
    if (send_context == NULL) {
        return -1;
    }
    send_context->value = g_bytes_new(data, len);
    if (send_context->value == NULL) {
        free(send_context);
        return -1;
    }
    send_context->ble = ble;
    g_main_context_invoke(ble->main_context, blufi_ble_send_in_loop, send_context);
    return 0;
}

int blufi_ble_disconnect(blufi_ble *ble)
{
    if (ble == NULL || ble->main_context == NULL) {
        return -1;
    }
    g_main_context_invoke(ble->main_context, blufi_ble_disconnect_in_loop, ble);
    return 0;
}

void blufi_ble_destroy(blufi_ble *ble)
{
    if (ble == NULL) {
        return;
    }
    if (ble->thread_started) {
        if (ble->main_context != NULL) {
            g_main_context_invoke(ble->main_context, blufi_ble_quit_in_loop, ble);
        }
        (void)pthread_join(ble->thread, NULL);
    }
    (void)pthread_cond_destroy(&ble->ready_condition);
    (void)pthread_mutex_destroy(&ble->lock);
    free(ble);
}
