/*
 * mdrd - MDR daemon
 *
 *  Copyright (C) 2021 Andreas Olofsson
 *
 *
 * This file is part of mdrd.
 *
 * mdrd is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * mdr is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with mdrd. If not, see <https://www.gnu.org/licenses/>.
 */

#include "device.h"

#include "mdr/device.h"
#include "mdr_device_ifaces.h"

#include <signal.h>

extern GDBusConnection* connection;

typedef struct device_source device_source_t;

struct device
{
    int ref_count;
    const gchar* dbus_name;
    mdr_device_t* mdr_device;

    device_source_t* source;

    int registrations_in_progress;

    OrgMdrDevice* device_iface;
    OrgMdrPowerOff* power_off_iface;
    OrgMdrBattery* battery_iface;
    OrgMdrLeftRightBattery* left_right_battery_iface;
    OrgMdrCradleBattery* cradle_battery_iface;
    OrgMdrLeftRight* left_right_iface;
    OrgMdrNoiseCancelling* noise_cancelling_iface;
    OrgMdrAmbientSoundMode* ambient_sound_mode_iface;
    OrgMdrEq* eq_iface;
    OrgMdrAutoPowerOff* auto_power_off_iface;

    uint8_t asm_amount;
    bool asm_voice;

    uint8_t eq_band_count;
    uint8_t eq_level_steps;
    const gchar* eq_presets[0x100];
};

GHashTable* device_table;

static void device_removed(device_t* device);

static void device_ref(device_t* device);

static void device_unref(device_t* device);

struct device_source
{
    GSource source;
    GPollFD poll_fd;
    device_t* device;
};

void devices_init(void)
{
    device_table = g_hash_table_new_full(
            g_str_hash,
            g_str_equal,
            g_free,
            (void (*)(void*)) device_removed);
}

void devices_deinit(void)
{
    g_hash_table_destroy(device_table);
}

typedef struct
{
    device_t* device;

    device_created_cb success_cb;
    device_create_error_cb error_cb;
    void* user_data;
}
device_add_init_data;

static gboolean device_source_prepare(GSource*, gint* timeout);
static gboolean device_source_check(GSource*);
static gboolean device_source_dispatch(GSource*, GSourceFunc, gpointer);

static void device_source_dispose(GSource* source);

static GSourceFuncs device_source_funcs = {
    .prepare = device_source_prepare,
    .check = device_source_check,
    .dispatch = device_source_dispatch,
};

static void device_add_init_success(void* user_data);

static void device_add_init_error(void* user_data);

void device_add(const gchar* name,
                gint sock,
                device_created_cb success_cb,
                device_create_error_cb error_cb,
                void* user_data)
{
    device_t* device = malloc(sizeof(device_t));
    if (device == NULL)
    {
        error_cb(user_data);
        return;
    }
    device_add_init_data* init_data = malloc(sizeof(device_add_init_data));
    if (init_data == NULL)
    {
        free(device);
        error_cb(user_data);
        return;
    }

    mdr_device_t* mdr_device = mdr_device_new_from_sock(sock);
    if (mdr_device == NULL)
    {
        error_cb(user_data);
        return;
    }

    g_debug("Connected to MDR device '%s'", name);

    device->ref_count = 2; // Initialization/table + source
    device->dbus_name = g_strdup(name);
    device->mdr_device = mdr_device;

    device->registrations_in_progress = 0;

    device->device_iface = NULL;
    device->power_off_iface = NULL;
    device->battery_iface = NULL;
    device->left_right_battery_iface = NULL;
    device->cradle_battery_iface = NULL;
    device->left_right_iface = NULL;
    device->noise_cancelling_iface = NULL;
    device->ambient_sound_mode_iface = NULL;
    device->eq_iface = NULL;
    device->auto_power_off_iface = NULL;

    memset(&device->eq_presets, 0, sizeof(gchar*) * 0x100);

    init_data->device = device;

    init_data->success_cb = success_cb;
    init_data->error_cb = error_cb;
    init_data->user_data = user_data;

    mdr_device_init(mdr_device,
                    device_add_init_success,
                    device_add_init_error,
                    init_data);

    device_source_t* dev_source =
            (device_source_t*) g_source_new(&device_source_funcs,
                                            sizeof(device_source_t));
    dev_source->poll_fd.fd = sock;
    dev_source->poll_fd.events = G_IO_IN | G_IO_OUT | G_IO_ERR | G_IO_HUP;
    dev_source->poll_fd.revents = 0;
    dev_source->device = device;

    device->source = dev_source;

    g_source_set_dispose_function(&dev_source->source, device_source_dispose);

    g_source_add_poll(&dev_source->source, &dev_source->poll_fd);

    g_source_attach(&dev_source->source, g_main_context_default());
    // TODO add source destory func
}

static void device_add_init_name_success(uint8_t len,
                                         const uint8_t* name,
                                         void* user_data);

static void device_add_init_success(void* user_data)
{
    device_add_init_data* init_data = user_data;
    device_t* device = init_data->device;

    g_debug("Device '%s' initialized", device->dbus_name);

    mdr_device_get_model_name(
            device->mdr_device,
            device_add_init_name_success,
            device_add_init_error,
            user_data);
}

static void device_start_registration(device_t*);
static void device_finish_registration(device_t*);

static void device_init_power_off(device_t*);
static void device_init_battery(device_t*);
static void device_init_left_right_battery(device_t*);
static void device_init_cradle_battery(device_t*);
static void device_init_left_right_connection_status(device_t*);
static void device_init_noise_cancelling(device_t*);
static void device_init_ambient_sound_mode(device_t*);
static void device_init_eq(device_t* device);
static void device_init_auto_power_off(device_t* device);

static void device_add_init_name_success(uint8_t len,
                                         const uint8_t* name,
                                         void* user_data)
{
    device_add_init_data* init_data = user_data;
    device_t* device = init_data->device;

    g_debug("Got name for device '%s'", device->dbus_name);

    device->device_iface = org_mdr_device_skeleton_new();

    GError* error = NULL;

    if (g_dbus_interface_skeleton_export(
            G_DBUS_INTERFACE_SKELETON(device->device_iface),
            connection,
            device->dbus_name,
            &error))
    {
        g_dbus_interface_skeleton_flush(
                G_DBUS_INTERFACE_SKELETON(device->device_iface));

        org_mdr_device_set_name(
                device->device_iface,
                g_strndup((gchar*) name, len));

        g_debug("Registered device interface for '%s'", device->dbus_name);
    }
    else
    {
        g_warning("Failed to register device interface: "
                  "%s", error->message);
        
        init_data->error_cb(init_data->user_data);

        // TODO cleanup device

        return;
    }

    g_hash_table_insert(device_table, g_strdup(device->dbus_name), device);

    init_data->success_cb(init_data->user_data);
    free(init_data);

    mdr_device_supported_functions_t supported_functions
            = mdr_device_get_supported_functions(device->mdr_device);

    if (supported_functions.power_off)
        device_init_power_off(device);

    if (supported_functions.battery)
        device_init_battery(device);

    if (supported_functions.left_right_battery)
        device_init_left_right_battery(device);

    if (supported_functions.left_right_connection_status)
        device_init_left_right_connection_status(device);

    if (supported_functions.cradle_battery)
        device_init_cradle_battery(device);

    if (supported_functions.noise_cancelling)
        device_init_noise_cancelling(device);

    if (supported_functions.ambient_sound_mode)
        device_init_ambient_sound_mode(device);

    if (supported_functions.eq || supported_functions.eq_non_customizable)
        device_init_eq(device);

    if (supported_functions.auto_power_off)
        device_init_auto_power_off(device);

    if (device->registrations_in_progress == 0)
    {
        org_mdr_device_emit_connected(device->device_iface);
    }
}

static void device_start_registration(device_t* device)
{
    device->registrations_in_progress++;
}

static void device_finish_registration(device_t* device)
{
    device->registrations_in_progress--;

    if (device->registrations_in_progress == 0)
    {
        org_mdr_device_emit_connected(device->device_iface);
    }
}

static gboolean device_handle_power_off(
        OrgMdrNoiseCancelling* interface,
        GDBusMethodInvocation* invocation,
        gpointer user_data);

static void device_init_power_off(device_t* device)
{
    device->power_off_iface = org_mdr_power_off_skeleton_new();

    GError* error = NULL;

    if (g_dbus_interface_skeleton_export(
            G_DBUS_INTERFACE_SKELETON(device->power_off_iface),
            connection,
            device->dbus_name,
            &error))
    {
        g_dbus_interface_skeleton_flush(
                G_DBUS_INTERFACE_SKELETON(device->power_off_iface));

        g_signal_connect(device->power_off_iface,
                         "handle-power-off",
                         G_CALLBACK(device_handle_power_off),
                         device);
    }
    else
    {
        g_warning("Failed to register power off interface: "
                  "%s", error->message);
    }
}

static void device_handle_power_off_success(void* user_data);

static void device_handle_power_off_error(void* user_data);

static gboolean device_handle_power_off(
        OrgMdrNoiseCancelling* interface,
        GDBusMethodInvocation* invocation,
        gpointer user_data)
{
    device_t* device = user_data;

    if (mdr_device_power_off(
            device->mdr_device,
            device_handle_power_off_success,
            device_handle_power_off_error,
            invocation) < 0)
    {
        g_dbus_method_invocation_return_dbus_error(
                invocation,
                "org.mdr.DeviceError",
                "Failed to make call.");
    }

    return TRUE;
}

static void device_handle_power_off_success(void* user_data)
{
    GDBusMethodInvocation* invocation = user_data;

    g_dbus_method_invocation_return_value(invocation, NULL);
}

static void device_handle_power_off_error(void* user_data)
{
    GDBusMethodInvocation* invocation = user_data;

    g_dbus_method_invocation_return_dbus_error(
            invocation,
            "org.mdr.DeviceError",
            "Call failed.");
}

static void device_init_battery_success(uint8_t level,
                                        bool charging,
                                        void* user_data);

static void device_init_battery_error(void* user_data);

static void device_init_battery(device_t* device)
{
    if (mdr_device_get_battery_level(
            device->mdr_device,
            device_init_battery_success,
            device_init_battery_error,
            device) < 0)
    {
        g_warning("Device init battery failed: %d", errno);
    }
    else
    {
        device_start_registration(device);
        device_ref(device);
    }
}

static void device_init_battery_error(void* user_data)
{
    device_t* device = user_data;

    g_warning("Device init battery failed: %d", errno);

    device_finish_registration(device);
    device_unref(device);
}

static void device_battery_update(uint8_t level,
                                  bool charging,
                                  void* user_data);

static void device_init_battery_success(uint8_t level,
                                        bool charging,
                                        void* user_data)
{
    device_t* device = user_data;

    device->battery_iface = org_mdr_battery_skeleton_new();

    GError* error = NULL;

    if (g_dbus_interface_skeleton_export(
            G_DBUS_INTERFACE_SKELETON(device->battery_iface),
            connection,
            device->dbus_name,
            &error))
    {
        g_dbus_interface_skeleton_flush(
                G_DBUS_INTERFACE_SKELETON(device->battery_iface));

        org_mdr_battery_set_level(device->battery_iface, level);
        org_mdr_battery_set_charging(device->battery_iface, charging);

        g_debug("Registered battery interface for '%s'", device->dbus_name);

        mdr_device_subscribe_battery_level(
                device->mdr_device,
                device_battery_update,
                device);
    }
    else
    {
        g_warning("Failed to register battery interface: "
                  "%s", error->message);
    }

    device_finish_registration(device);
    device_unref(device);
}

static void device_battery_update(uint8_t level,
                                  bool charging,
                                  void* user_data)
{
    device_t* device = user_data;

    if (device->battery_iface != NULL)
    {
        org_mdr_battery_set_level(device->battery_iface, level);
        org_mdr_battery_set_charging(device->battery_iface, charging);
    }
}

static void device_init_left_right_battery_success(uint8_t left_level,
                                                   bool left_charging,
                                                   uint8_t right_level,
                                                   bool right_charging,
                                                   void* user_data);

static void device_init_left_right_battery_error(void* user_data);

static void device_init_left_right_battery(device_t* device)
{
    if (mdr_device_get_left_right_battery_level(
            device->mdr_device,
            device_init_left_right_battery_success,
            device_init_left_right_battery_error,
            device) < 0)
    {
        g_warning("Device init left-right battery failed: %d", errno);
    }
    else
    {
        device_start_registration(device);
        device_ref(device);
    }
}

static void device_init_left_right_battery_error(void* user_data)
{
    device_t* device = user_data;

    g_warning("Device init left-right battery failed: %d", errno);
    
    device_finish_registration(device);
    device_unref(device);
}

static void device_left_right_battery_update(uint8_t left_level,
                                             bool left_charging,
                                             uint8_t right_level,
                                             bool right_charging,
                                             void* user_data);

static void device_init_left_right_battery_success(uint8_t left_level,
                                                   bool left_charging,
                                                   uint8_t right_level,
                                                   bool right_charging,
                                                   void* user_data)
{
    device_t* device = user_data;

    device->left_right_battery_iface
        = org_mdr_left_right_battery_skeleton_new();

    GError* error = NULL;

    if (g_dbus_interface_skeleton_export(
            G_DBUS_INTERFACE_SKELETON(device->left_right_battery_iface),
            connection,
            device->dbus_name,
            &error))
    {
        g_dbus_interface_skeleton_flush(
                G_DBUS_INTERFACE_SKELETON(device->left_right_battery_iface));

        org_mdr_left_right_battery_set_left_level(
                device->left_right_battery_iface,
                left_level);
        org_mdr_left_right_battery_set_right_level(
                device->left_right_battery_iface,
                right_level);
        org_mdr_left_right_battery_set_left_charging(
                device->left_right_battery_iface,
                left_charging);
        org_mdr_left_right_battery_set_right_charging(
                device->left_right_battery_iface,
                right_charging);

        g_debug("Registered left-right battery interface for '%s'",
                device->dbus_name);

        mdr_device_subscribe_left_right_battery_level(
                device->mdr_device,
                device_left_right_battery_update,
                device);
    }
    else
    {
        g_warning("Failed to register left-right battery interface: "
                  "%s", error->message);
    }

    device_finish_registration(device);
    device_unref(device);
}

static void device_left_right_battery_update(uint8_t left_level,
                                             bool left_charging,
                                             uint8_t right_level,
                                             bool right_charging,
                                             void* user_data)
{
    device_t* device = user_data;

    if (device->left_right_battery_iface != NULL)
    {
        org_mdr_left_right_battery_set_left_level(
                device->left_right_battery_iface,
                left_level);
        org_mdr_left_right_battery_set_right_level(
                device->left_right_battery_iface,
                right_level);
        org_mdr_left_right_battery_set_left_charging(
                device->left_right_battery_iface,
                left_charging);
        org_mdr_left_right_battery_set_right_charging(
                device->left_right_battery_iface,
                right_charging);
    }
}

static void device_init_cradle_battery_success(uint8_t level,
                                               bool charging,
                                               void* user_data);

static void device_init_cradle_battery_error(void* user_data);

static void device_init_cradle_battery(device_t* device)
{
    if (mdr_device_get_cradle_battery_level(
            device->mdr_device,
            device_init_cradle_battery_success,
            device_init_cradle_battery_error,
            device) < 0)
    {
        g_warning("Device init cradle battery failed: %d", errno);
    }
    else
    {
        device_start_registration(device);
        device_ref(device);
    }
}

static void device_init_cradle_battery_error(void* user_data)
{
    device_t* device = user_data;

    g_warning("Device init cradle battery failed: %d", errno);
    
    device_finish_registration(device);
    device_unref(device);
}

static void device_cradle_battery_update(uint8_t level,
                                  bool charging,
                                  void* user_data);

static void device_init_cradle_battery_success(uint8_t level,
                                               bool charging,
                                               void* user_data)
{
    device_t* device = user_data;

    device->cradle_battery_iface = org_mdr_cradle_battery_skeleton_new();

    GError* error = NULL;

    if (g_dbus_interface_skeleton_export(
            G_DBUS_INTERFACE_SKELETON(device->cradle_battery_iface),
            connection,
            device->dbus_name,
            &error))
    {
        g_dbus_interface_skeleton_flush(
                G_DBUS_INTERFACE_SKELETON(device->cradle_battery_iface));

        org_mdr_cradle_battery_set_level(device->cradle_battery_iface, level);
        org_mdr_cradle_battery_set_charging(device->cradle_battery_iface,
                                            charging);

        g_debug("Registered cradle battery interface for '%s'", device->dbus_name);

        mdr_device_subscribe_cradle_battery_level(
                device->mdr_device,
                device_cradle_battery_update,
                device);
    }
    else
    {
        g_warning("Failed to register cradle battery interface: "
                  "%s", error->message);
    }

    device_finish_registration(device);
    device_unref(device);
}

static void device_cradle_battery_update(uint8_t level,
                                         bool charging,
                                         void* user_data)
{
    device_t* device = user_data;

    if (device->cradle_battery_iface != NULL)
    {
        org_mdr_cradle_battery_set_level(device->cradle_battery_iface, level);
        org_mdr_cradle_battery_set_charging(device->cradle_battery_iface,
                                            charging);
    }
}

static void device_init_left_right_connection_status_success(
        bool left_connected,
        bool right_connected,
        void* user_data);

static void device_init_left_right_connection_status_error(void* user_data);

static void device_init_left_right_connection_status(device_t* device)
{
    if (mdr_device_get_left_right_connection_status(
            device->mdr_device,
            device_init_left_right_connection_status_success,
            device_init_left_right_connection_status_error,
            device) < 0)
    {
        g_warning("Device init left-right connection status failed: %d", errno);
    }
    else
    {
        device_start_registration(device);
        device_ref(device);
    }
}

static void device_init_left_right_connection_status_error(void* user_data)
{
    device_t* device = user_data;

    g_warning("Device init left-right connection status failed: %d", errno);
    
    device_finish_registration(device);
    device_unref(device);
}

static void device_left_right_connection_status_update(bool left_connected,
                                                       bool right_connected,
                                                       void* user_data);

static void device_init_left_right_connection_status_success(
        bool left_connected,
        bool right_connected,
        void* user_data)
{
    device_t* device = user_data;

    device->left_right_iface = org_mdr_left_right_skeleton_new();

    GError* error = NULL;

    if (g_dbus_interface_skeleton_export(
            G_DBUS_INTERFACE_SKELETON(device->left_right_iface),
            connection,
            device->dbus_name,
            &error))
    {
        g_dbus_interface_skeleton_flush(
                G_DBUS_INTERFACE_SKELETON(device->left_right_iface));

        org_mdr_left_right_set_left_connected(device->left_right_iface,
                                              left_connected);
        org_mdr_left_right_set_right_connected(device->left_right_iface,
                                               right_connected);

        g_debug("Registered left-right interface for '%s'", device->dbus_name);

        mdr_device_subscribe_left_right_connection_status(
                device->mdr_device,
                device_left_right_connection_status_update,
                device);
    }
    else
    {
        g_warning("Failed to register left-right interface: "
                  "%s", error->message);
    }

    device_finish_registration(device);
    device_unref(device);
}

static void device_left_right_connection_status_update(bool left_connected,
                                                       bool right_connected,
                                                       void* user_data)
{
    device_t* device = user_data;

    if (device->left_right_iface != NULL)
    {
        org_mdr_left_right_set_left_connected(device->left_right_iface,
                                              left_connected);
        org_mdr_left_right_set_right_connected(device->left_right_iface,
                                               right_connected);
    }
}

static void device_init_noise_cancelling_success(bool enabled,
                                                 void* user_data);

static void device_init_noise_cancelling_error(void* user_data);

static void device_init_noise_cancelling(device_t* device)
{
    if (mdr_device_get_noise_cancelling_enabled(
                device->mdr_device,
                device_init_noise_cancelling_success,
                device_init_noise_cancelling_error,
                device) < 0)
    {
        g_warning("Device init noise cancelling failed: %d", errno);
    }
    else
    {
        device_start_registration(device);
        device_ref(device);
    }
}

static void device_init_noise_cancelling_error(void* user_data)
{
    device_t* device = user_data;

    g_warning("Device init noise cancelling failed: %d", errno);
    
    device_finish_registration(device);
    device_unref(device);
}

static void device_noise_cancelling_update(bool enabled,
                                           void* user_data);

static gboolean device_noise_cancelling_enable(
        OrgMdrNoiseCancelling* interface,
        GDBusMethodInvocation* invocation,
        gpointer user_data);

static gboolean device_noise_cancelling_disable(
        OrgMdrNoiseCancelling* interface,
        GDBusMethodInvocation* invocation,
        gpointer user_data);

static void device_init_noise_cancelling_success(bool enabled,
                                                 void* user_data)
{
    device_t* device = user_data;

    device->noise_cancelling_iface = org_mdr_noise_cancelling_skeleton_new();

    g_signal_connect(device->noise_cancelling_iface,
                     "handle-enable",
                     G_CALLBACK(device_noise_cancelling_enable),
                     device);

    g_signal_connect(device->noise_cancelling_iface,
                     "handle-disable",
                     G_CALLBACK(device_noise_cancelling_disable),
                     device);

    GError* error = NULL;

    if (g_dbus_interface_skeleton_export(
            G_DBUS_INTERFACE_SKELETON(device->noise_cancelling_iface),
            connection,
            device->dbus_name,
            &error))
    {
        g_dbus_interface_skeleton_flush(
                G_DBUS_INTERFACE_SKELETON(device->noise_cancelling_iface));

        org_mdr_noise_cancelling_set_enabled(device->noise_cancelling_iface,
                                             enabled);

        g_debug("Registered noise cancelling interface for '%s'",
                device->dbus_name);

        mdr_device_subscribe_noise_cancelling_enabled(
                device->mdr_device,
                device_noise_cancelling_update,
                device);
    }
    else
    {
        g_warning("Failed to noise cancelling interface: "
                  "%s", error->message);
    }

    device_finish_registration(device);
    device_unref(device);
}

static void device_noise_cancelling_enable_success(void*);

static void device_noise_cancelling_enable_error(void*);

static gboolean device_noise_cancelling_enable(
        OrgMdrNoiseCancelling* interface,
        GDBusMethodInvocation* invocation,
        gpointer user_data)
{
    device_t* device = user_data;

    mdr_device_enable_noise_cancelling(
            device->mdr_device,
            device_noise_cancelling_enable_success,
            device_noise_cancelling_enable_error,
            invocation);

    return TRUE;
}

static void device_noise_cancelling_enable_success(void* user_data)
{
    GDBusMethodInvocation* invocation = user_data;

    g_dbus_method_invocation_return_value(invocation, NULL);
}

static void device_noise_cancelling_enable_error(void* user_data)
{
    GDBusMethodInvocation* invocation = user_data;

    g_dbus_method_invocation_return_dbus_error(
            invocation,
            "org.mdr.DeviceError",
            "Call failed");
}

static void device_noise_cancelling_disable_success(void*);

static void device_noise_cancelling_disable_error(void*);

static gboolean device_noise_cancelling_disable(
        OrgMdrNoiseCancelling* interface,
        GDBusMethodInvocation* invocation,
        gpointer user_data)
{
    device_t* device = user_data;

    mdr_device_disable_ncasm(
            device->mdr_device,
            device_noise_cancelling_disable_success,
            device_noise_cancelling_disable_error,
            invocation);

    return TRUE;
}

static void device_noise_cancelling_disable_success(void* user_data)
{
    GDBusMethodInvocation* invocation = user_data;

    g_dbus_method_invocation_return_value(invocation, NULL);
}

static void device_noise_cancelling_disable_error(void* user_data)
{
    GDBusMethodInvocation* invocation = user_data;

    g_dbus_method_invocation_return_dbus_error(
            invocation,
            "org.mdr.DeviceError",
            "Call failed");
}

static void device_noise_cancelling_update(bool enabled,
                                           void* user_data)
{
    device_t* device = user_data;

    if (device->noise_cancelling_iface != NULL)
    {
        org_mdr_noise_cancelling_set_enabled(device->noise_cancelling_iface,
                                             enabled);
    }
}

static void device_init_ambient_sound_mode_success(uint8_t amount,
                                                   bool voice,
                                                   void* user_data);

static void device_init_ambient_sound_mode_error(void* user_data);

static void device_init_ambient_sound_mode(device_t* device)
{
    if (mdr_device_get_ambient_sound_mode_settings(
                device->mdr_device,
                device_init_ambient_sound_mode_success,
                device_init_ambient_sound_mode_error,
                device) < 0)
    {
        g_warning("Device init ambient sound mode failed: %d", errno);
    }
    else
    {
        device_start_registration(device);
        device_ref(device);
    }
}

static void device_init_ambient_sound_mode_error(void* user_data)
{
    device_t* device = user_data;

    g_warning("Device init ambient sound mode failed: %d", errno);
    
    device_finish_registration(device);
    device_unref(device);
}

static void device_ambient_sound_mode_update(uint8_t amount,
                                             bool voice,
                                             void* user_data);

static gboolean device_ambient_sound_mode_set_amount(
        OrgMdrNoiseCancelling* interface,
        GDBusMethodInvocation* invocation,
        guint32 amount,
        gpointer user_data);

static gboolean device_ambient_sound_mode_set_mode(
        OrgMdrNoiseCancelling* interface,
        GDBusMethodInvocation* invocation,
        const gchar* name,
        gpointer user_data);

static void device_init_ambient_sound_mode_success(uint8_t amount,
                                                   bool voice,
                                                   void* user_data)
{
    device_t* device = user_data;

    device->asm_amount = amount;
    device->asm_voice = voice;

    device->ambient_sound_mode_iface
        = org_mdr_ambient_sound_mode_skeleton_new();

    g_signal_connect(device->ambient_sound_mode_iface,
                     "handle-set-amount",
                     G_CALLBACK(device_ambient_sound_mode_set_amount),
                     device);

    g_signal_connect(device->ambient_sound_mode_iface,
                     "handle-set-mode",
                     G_CALLBACK(device_ambient_sound_mode_set_mode),
                     device);

    GError* error = NULL;

    if (g_dbus_interface_skeleton_export(
            G_DBUS_INTERFACE_SKELETON(device->ambient_sound_mode_iface),
            connection,
            device->dbus_name,
            &error))
    {
        g_dbus_interface_skeleton_flush(
                G_DBUS_INTERFACE_SKELETON(device->ambient_sound_mode_iface));

        org_mdr_ambient_sound_mode_set_amount(device->ambient_sound_mode_iface,
                                              amount);
        org_mdr_ambient_sound_mode_set_mode(device->ambient_sound_mode_iface,
                                            voice ? "voice" : "normal");

        g_debug("Registered ambient sound mode interface for '%s'",
                device->dbus_name);

        mdr_device_subscribe_ambient_sound_mode_settings(
                device->mdr_device,
                device_ambient_sound_mode_update,
                device);
    }
    else
    {
        g_warning("Failed to ambient sound mode interface: "
                  "%s", error->message);
    }

    device_finish_registration(device);
    device_unref(device);
}

static void device_ambient_sound_mode_set_amount_success(void*);

static void device_ambient_sound_mode_set_amount_error(void*);

static gboolean device_ambient_sound_mode_set_amount(
        OrgMdrNoiseCancelling* interface,
        GDBusMethodInvocation* invocation,
        guint32 amount,
        gpointer user_data)
{
    device_t* device = user_data;

    mdr_device_enable_ambient_sound_mode(
            device->mdr_device,
            amount > 0xff ? 0xff : amount,
            device->asm_voice,
            device_ambient_sound_mode_set_amount_success,
            device_ambient_sound_mode_set_amount_error,
            invocation);

    return TRUE;
}

static void device_ambient_sound_mode_set_amount_success(void* user_data)
{
    GDBusMethodInvocation* invocation = user_data;

    g_dbus_method_invocation_return_value(invocation, NULL);
}

static void device_ambient_sound_mode_set_amount_error(void* user_data)
{
    GDBusMethodInvocation* invocation = user_data;

    g_dbus_method_invocation_return_dbus_error(invocation,
                                               "org.mdr.DeviceError",
                                               "Call failed");
}

static void device_ambient_sound_mode_set_mode_success(void*);

static void device_ambient_sound_mode_set_mode_error(void*);

static gboolean device_ambient_sound_mode_set_mode(
        OrgMdrNoiseCancelling* interface,
        GDBusMethodInvocation* invocation,
        const gchar* name,
        gpointer user_data)
{
    device_t* device = user_data;

    bool voice;

    if (!(voice = g_str_equal(name, "voice")) && !g_str_equal(name, "normal"))
    {
        g_dbus_method_invocation_return_dbus_error(invocation,
                                                   "org.mdr.InvalidASMMode",
                                                   "Invalid ASM mode, "
                                                   "valid modes are: "
                                                   "'voice' and 'normal'.");
        return TRUE;
    }

    mdr_device_enable_ambient_sound_mode(
            device->mdr_device,
            device->asm_amount,
            voice,
            device_ambient_sound_mode_set_mode_success,
            device_ambient_sound_mode_set_mode_error,
            invocation);

    return TRUE;
}

static void device_ambient_sound_mode_set_mode_success(void* user_data)
{
    GDBusMethodInvocation* invocation = user_data;

    g_dbus_method_invocation_return_value(invocation, NULL);
}

static void device_ambient_sound_mode_set_mode_error(void* user_data)
{
    GDBusMethodInvocation* invocation = user_data;

    g_dbus_method_invocation_return_dbus_error(invocation,
                                               "org.mdr.DeviceError",
                                               "Call failed");
}

static void device_ambient_sound_mode_update(uint8_t amount,
                                             bool voice,
                                             void* user_data)
{
    device_t* device = user_data;

    device->asm_amount = amount;
    device->asm_voice = voice;

    if (device->ambient_sound_mode_iface != NULL)
    {
        org_mdr_ambient_sound_mode_set_amount(device->ambient_sound_mode_iface,
                                              amount);
        org_mdr_ambient_sound_mode_set_mode(device->ambient_sound_mode_iface,
                                            voice ? "voice" : "normal");
    }
}

static void device_init_eq_get_capabilities_result(
        uint8_t band_count,
        uint8_t level_steps,
        uint8_t num_presets,
        mdr_packet_eqebb_eq_preset_id_t* presets,
        void* user_data);

static void device_init_eq_error(void* user_data);

static void device_init_eq(device_t* device)
{
    if (mdr_device_get_eq_capabilities(
            device->mdr_device,
            device_init_eq_get_capabilities_result,
            device_init_eq_error,
            device) < 0)
    {
        g_warning("Device init EQ failed: %d", errno);
    }
    else
    {
        device_start_registration(device);
        device_ref(device);
    }
}

static void device_init_eq_get_preset_and_levels_result(
        mdr_packet_eqebb_eq_preset_id_t,
        uint8_t num_levels,
        uint8_t* levels,
        void* user_data);

static void device_init_eq_get_capabilities_result(
        uint8_t band_count,
        uint8_t level_steps,
        uint8_t num_presets,
        mdr_packet_eqebb_eq_preset_id_t* presets,
        void* user_data)
{
    device_t* device = user_data;

    device->eq_band_count = band_count;
    device->eq_level_steps = level_steps;

    for (int i = 0; i < num_presets; i++)
    {
        mdr_packet_eqebb_eq_preset_id_t preset = presets[i];

        const char* name = mdr_packet_eqebb_get_preset_name(preset);
        if (name == NULL)
        {
            continue;
        }

        device->eq_presets[preset] = name;
    }

    mdr_device_get_eq_preset_and_levels(
            device->mdr_device,
            device_init_eq_get_preset_and_levels_result,
            device_init_eq_error,
            device);
}

static gboolean device_eq_set_preset(
        OrgMdrNoiseCancelling* interface,
        GDBusMethodInvocation* invocation,
        const gchar* preset,
        gpointer user_data);

static gboolean device_eq_set_levels(
        OrgMdrNoiseCancelling* interface,
        GDBusMethodInvocation* invocation,
        GVariant* levels,
        gpointer user_data);

static void device_eq_preset_and_levels_update(
        mdr_packet_eqebb_eq_preset_id_t preset_id,
        uint8_t num_levels,
        uint8_t* levels,
        void* user_data);

static void device_init_eq_get_preset_and_levels_result(
        mdr_packet_eqebb_eq_preset_id_t preset_id,
        uint8_t num_levels,
        uint8_t* levels,
        void* user_data)
{
    device_t* device = user_data;

    device->eq_iface = org_mdr_eq_skeleton_new();

    g_signal_connect(device->eq_iface,
                     "handle-set-preset",
                     G_CALLBACK(device_eq_set_preset),
                     device);

    g_signal_connect(device->eq_iface,
                     "handle-set-levels",
                     G_CALLBACK(device_eq_set_levels),
                     device);

    GError* error = NULL;

    if (g_dbus_interface_skeleton_export(
            G_DBUS_INTERFACE_SKELETON(device->eq_iface),
            connection,
            device->dbus_name,
            &error))
    {
        g_dbus_interface_skeleton_flush(
                G_DBUS_INTERFACE_SKELETON(device->eq_iface));

        const gchar* preset_name = device->eq_presets[preset_id];

        if (preset_name == NULL)
        {
            preset_name = "<Unknown>";
        }

        int preset_count = 0;
        for (int i = 0; i < 0x100; i++)
        {
            if (device->eq_presets[i] != NULL)
                preset_count++;
        }

        const gchar** preset_names = g_malloc_n(preset_count + 1, sizeof(gchar*));
        for (int i = 0, j = 0; i < 0x100; i++)
        {
            if (device->eq_presets[i] != NULL)
            {
                preset_names[j] = device->eq_presets[i];
                j++;
            }
        }
        preset_names[preset_count] = 0;

        GVariantBuilder* levels_variant = g_variant_builder_new(G_VARIANT_TYPE("au"));

        for (int i = 0; i < num_levels; i++)
        {
            g_variant_builder_add(levels_variant, "u", (guint32) levels[i]);
        }

        org_mdr_eq_set_band_count(device->eq_iface, device->eq_band_count);
        org_mdr_eq_set_level_steps(device->eq_iface, device->eq_level_steps);
        org_mdr_eq_set_preset(device->eq_iface, preset_name);
        org_mdr_eq_set_available_presets(device->eq_iface, preset_names);
        org_mdr_eq_set_levels(device->eq_iface,
                              g_variant_builder_end(levels_variant));

        g_debug("Registered EQ interface for '%s'", device->dbus_name);

        g_free(preset_names);

        mdr_device_subscribe_eq_preset_and_levels(
                device->mdr_device,
                device_eq_preset_and_levels_update,
                device);
    }
    else
    {
        g_warning("Failed to register EQ interface: "
                  "%s", error->message);
    }

    device_finish_registration(device);
    device_unref(device);
}

static void device_init_eq_error(void* user_data)
{
    device_t* device = user_data;

    g_warning("Device init EQ failed: %d", errno);

    device_finish_registration(device);
    device_unref(device);
}

static void device_eq_set_preset_success(void* user_data);

static void device_eq_set_preset_error(void* user_data);

static gboolean device_eq_set_preset(
        OrgMdrNoiseCancelling* interface,
        GDBusMethodInvocation* invocation,
        const gchar* preset,
        gpointer user_data)
{
    device_t* device = user_data;

    mdr_packet_eqebb_eq_preset_id_t preset_id;
    bool preset_found = false;

    for (int i = 0; i < 0x100; i++)
    {
        if (device->eq_presets[i] != NULL
                && g_str_equal(device->eq_presets[i], preset))
        {
            preset_id = i;
            preset_found = true;
            break;
        }
    }

    if (!preset_found)
    {
        g_dbus_method_invocation_return_dbus_error(
                invocation,
                "org.mdr.InvalidValue",
                "Preset not found");
        return TRUE;
    }

    if (mdr_device_set_eq_preset(
            device->mdr_device,
            preset_id,
            device_eq_set_preset_success,
            device_eq_set_preset_error,
            invocation) < 0)
    {
        g_dbus_method_invocation_return_dbus_error(
                invocation,
                "org.mdr.DeviceError",
                "Failed to make the call.");
    }

    return TRUE;
}

static void device_eq_set_preset_success(void* user_data)
{
    GDBusMethodInvocation* invocation = user_data;

    g_dbus_method_invocation_return_value(invocation, NULL);
}

static void device_eq_set_preset_error(void* user_data)
{
    GDBusMethodInvocation* invocation = user_data;

    g_dbus_method_invocation_return_dbus_error(
            invocation,
            "org.mdr.DeviceError",
            "Call failed.");
}

static void device_eq_set_levels_success(void* user_data);

static void device_eq_set_levels_error(void* user_data);

static gboolean device_eq_set_levels(
        OrgMdrNoiseCancelling* interface,
        GDBusMethodInvocation* invocation,
        GVariant* levels_variant,
        gpointer user_data)
{
    device_t* device = user_data;

    gsize num_levels;
    const guint32* level_ints = g_variant_get_fixed_array(levels_variant,
                                                          &num_levels,
                                                          sizeof(guint32));

    if (num_levels != device->eq_band_count)
    {
        g_dbus_method_invocation_return_dbus_error(
                invocation,
                "org.mdr.InvalidValue",
                "The number of bands must match the device's.");
        return TRUE;
    }

    uint8_t* level_bytes = g_malloc_n(num_levels, sizeof(uint8_t));
    if (level_bytes == NULL)
    {
        // TODO
    }
    

    for (int i = 0; i < num_levels; i++)
    {
        if (level_ints[i] >= device->eq_level_steps)
        {
            g_dbus_method_invocation_return_dbus_error(
                    invocation,
                    "org.mdr.InvalidValue",
                    "Level not within range.");
            return TRUE;
        }

        level_bytes[i] = level_ints[i];
    }

    if (device->eq_iface)
    {
        org_mdr_eq_set_levels(device->eq_iface, levels_variant);
    }

    mdr_device_set_eq_levels(
            device->mdr_device,
            num_levels,
            level_bytes,
            device_eq_set_levels_success,
            device_eq_set_levels_error,
            invocation);

    free(level_bytes);

    return TRUE;
}

static void device_eq_set_levels_success(void* user_data)
{
    GDBusMethodInvocation* invocation = user_data;

    g_dbus_method_invocation_return_value(invocation, NULL);
}

static void device_eq_set_levels_error(void* user_data)
{
    GDBusMethodInvocation* invocation = user_data;

    g_dbus_method_invocation_return_dbus_error(
            invocation,
            "org.mdr.DeviceError",
            "Call failed.");
}

static void device_eq_preset_and_levels_update(
        mdr_packet_eqebb_eq_preset_id_t preset_id,
        uint8_t num_levels,
        uint8_t* levels,
        void* user_data)
{
    device_t* device = user_data;

    if (device->eq_iface != NULL)
    {
        const gchar* preset_name = device->eq_presets[preset_id];

        if (preset_name == NULL)
        {
            preset_name = "<Unknown>";
        }

        GVariantBuilder* levels_variant = g_variant_builder_new(G_VARIANT_TYPE("au"));

        for (int i = 0; i < num_levels; i++)
        {
            g_variant_builder_add(levels_variant, "u", (guint32) levels[i]);
        }

        org_mdr_eq_set_preset(device->eq_iface, preset_name);
        org_mdr_eq_set_levels(device->eq_iface,
                              g_variant_builder_end(levels_variant));
    }
}

static void device_init_auto_power_off_result(
        bool enabled,
        mdr_packet_system_auto_power_off_element_id_t timeout,
        void* user_data);

static void device_init_auto_power_off_error(void* user_data);

static void device_init_auto_power_off(device_t* device)
{
    if (mdr_device_setting_get_auto_power_off(
            device->mdr_device,
            device_init_auto_power_off_result,
            device_init_auto_power_off_error,
            device) < 0)
    {
        g_warning("Device init auto power off failed (1): %d", errno);
    }
    else
    {
        device_start_registration(device);
        device_ref(device);
    }
}

static const gchar* auto_power_off_timeout_to_string(
        mdr_packet_system_auto_power_off_element_id_t timeout)
{
    switch (timeout)
    {
        case MDR_PACKET_SYSTEM_AUTO_POWER_OFF_ELEMENT_ID_POWER_OFF_IN_5_MIN:
            return "5 min";

        case MDR_PACKET_SYSTEM_AUTO_POWER_OFF_ELEMENT_ID_POWER_OFF_IN_30_MIN:
            return "30 min";

        case MDR_PACKET_SYSTEM_AUTO_POWER_OFF_ELEMENT_ID_POWER_OFF_IN_60_MIN:
            return "60 min";

        case MDR_PACKET_SYSTEM_AUTO_POWER_OFF_ELEMENT_ID_POWER_OFF_IN_180_MIN:
            return "180 min";

        default:
            return NULL;
    }
}

static gboolean device_auto_power_off_set_timeout(
        OrgMdrNoiseCancelling* interface,
        GDBusMethodInvocation* invocation,
        const gchar* timeout,
        gpointer user_data);

static void device_auto_power_off_update(
        bool enabled,
        mdr_packet_system_auto_power_off_element_id_t timeout,
        void* user_data);

static void device_init_auto_power_off_result(
        bool enabled,
        mdr_packet_system_auto_power_off_element_id_t timeout,
        void* user_data)
{
    device_t* device = user_data;

    device->auto_power_off_iface = org_mdr_auto_power_off_skeleton_new();

    g_signal_connect(device->auto_power_off_iface,
                     "handle-set-timeout",
                     G_CALLBACK(device_auto_power_off_set_timeout),
                     device);

    GError* error = NULL;

    if (g_dbus_interface_skeleton_export(
                G_DBUS_INTERFACE_SKELETON(device->auto_power_off_iface),
                connection,
                device->dbus_name,
                &error))
    {
        const gchar* timeouts[5] = {
            "5 min",
            "30 min",
            "60 min",
            "180 min",
        };

        org_mdr_auto_power_off_set_available_timeouts(
                device->auto_power_off_iface,
                timeouts);

        if (enabled)
        {
            const gchar* timeout_str = auto_power_off_timeout_to_string(timeout);

            if (timeout_str == NULL)
                timeout_str = "<Unknown>";

            org_mdr_auto_power_off_set_timeout(device->auto_power_off_iface,
                                               timeout_str);

        }
        else
        {
            org_mdr_auto_power_off_set_timeout(device->auto_power_off_iface,
                                               "Off");
        }

        g_debug("Registered auto power off interface for '%s'", device->dbus_name);

        mdr_device_setting_subscribe_auto_power_off(
                device->mdr_device,
                device_auto_power_off_update,
                device);
    }
    else
    {
        device->auto_power_off_iface = NULL;

        g_warning("Failed to register auto power off interface (5): "
                  "%s", error->message);
    }

    device_finish_registration(device);
    device_unref(device);
}

static void device_auto_power_off_set_timeout_success(void* user_data);

static void device_auto_power_off_set_timeout_error(void* user_data);

static gboolean device_auto_power_off_set_timeout(
        OrgMdrNoiseCancelling* interface,
        GDBusMethodInvocation* invocation,
        const gchar* timeout,
        gpointer user_data)
{
    device_t* device = user_data;

    mdr_packet_system_auto_power_off_element_id_t timeout_id;

    if (g_str_equal(timeout, "Off"))
    {
        mdr_device_setting_disable_auto_power_off(
                device->mdr_device,
                device_auto_power_off_set_timeout_success,
                device_auto_power_off_set_timeout_error,
                invocation);
    }
    else
    {
        if (g_str_equal(timeout, "5 min"))
            timeout_id = MDR_PACKET_SYSTEM_AUTO_POWER_OFF_ELEMENT_ID_POWER_OFF_IN_5_MIN;
        else if (g_str_equal(timeout, "30 min"))
            timeout_id = MDR_PACKET_SYSTEM_AUTO_POWER_OFF_ELEMENT_ID_POWER_OFF_IN_30_MIN;
        else if (g_str_equal(timeout, "60 min"))
            timeout_id = MDR_PACKET_SYSTEM_AUTO_POWER_OFF_ELEMENT_ID_POWER_OFF_IN_60_MIN;
        else if (g_str_equal(timeout, "180 min"))
            timeout_id = MDR_PACKET_SYSTEM_AUTO_POWER_OFF_ELEMENT_ID_POWER_OFF_IN_180_MIN;
        else
        {
            g_dbus_method_invocation_return_dbus_error(
                    invocation,
                    "org.mdr.InvalidValue",
                    "Invalid timeout");
            return TRUE;
        }

        mdr_device_setting_enable_auto_power_off(
                device->mdr_device,
                timeout_id,
                device_auto_power_off_set_timeout_success,
                device_auto_power_off_set_timeout_error,
                invocation);
    }

    return TRUE;
}


static void device_auto_power_off_set_timeout_success(void* user_data)
{
    GDBusMethodInvocation* invocation = user_data;

    g_dbus_method_invocation_return_value(invocation, NULL);
}

static void device_auto_power_off_set_timeout_error(void* user_data)
{
    GDBusMethodInvocation* invocation = user_data;

    g_dbus_method_invocation_return_dbus_error(
            invocation,
            "org.mdr.DeviceError",
            "Call failed.");
}

static void device_auto_power_off_update(
        bool enabled,
        mdr_packet_system_auto_power_off_element_id_t timeout,
        void* user_data)
{
    device_t* device = user_data;

    if (device->auto_power_off_iface)
    {
        if (enabled)
        {
            const gchar* timeout_str = auto_power_off_timeout_to_string(timeout);

            if (timeout_str == NULL)
                timeout_str = "<Unknown>";

            org_mdr_auto_power_off_set_timeout(device->auto_power_off_iface,
                                               timeout_str);

        }
        else
        {
            org_mdr_auto_power_off_set_timeout(device->auto_power_off_iface,
                                               "Off");
        }
    }
}

static void device_init_auto_power_off_error(void* user_data)
{
    device_t* device = user_data;

    device->auto_power_off_iface = NULL;
    g_warning("Device init auto power off failed (4): %d", errno);

    device_finish_registration(device);
    device_unref(device);
}

static void device_add_init_error(void* user_data)
{
    device_add_init_data* init_data = user_data;

    init_data->error_cb(init_data->user_data);
}

void device_remove(const gchar* name)
{
    g_hash_table_remove(device_table, name);
}

static void device_removed(device_t* device)
{
    mdr_device_close(device->mdr_device);
    device->mdr_device = NULL;

    g_source_destroy(&device->source->source);
    g_source_unref(&device->source->source);

    device_unref(device);
}

static void device_ref(device_t* device)
{
    device->ref_count++;

    g_debug("Ref %d", device->ref_count);
}

/*
 * Called when a device is removed from the device table.
 *
 * Should destroy any DBus interfaces and free the device.
 */
static void device_unref(device_t* device)
{
    device->ref_count--;

    g_debug("Unref %d", device->ref_count);

    if (device->ref_count <= 0)
    {
        org_mdr_device_emit_disconnected(device->device_iface);

        org_mdr_device_set_name(device->device_iface, "");
        g_dbus_interface_skeleton_flush(
                G_DBUS_INTERFACE_SKELETON(device->device_iface));

        g_dbus_interface_skeleton_unexport_from_connection(
                G_DBUS_INTERFACE_SKELETON(device->device_iface),
                connection);
        g_object_unref(device->device_iface);

        if (device->power_off_iface != NULL)
        {
            g_dbus_interface_skeleton_unexport_from_connection(
                    G_DBUS_INTERFACE_SKELETON(device->power_off_iface),
                    connection);
            g_object_unref(device->power_off_iface);
        }

        if (device->battery_iface != NULL)
        {
            g_dbus_interface_skeleton_unexport_from_connection(
                    G_DBUS_INTERFACE_SKELETON(device->battery_iface),
                    connection);
            g_object_unref(device->battery_iface);
        }

        if (device->left_right_battery_iface != NULL)
        {
            g_dbus_interface_skeleton_unexport_from_connection(
                    G_DBUS_INTERFACE_SKELETON(device->left_right_battery_iface),
                    connection);
            g_object_unref(device->left_right_battery_iface);
        }

        if (device->cradle_battery_iface != NULL)
        {
            g_dbus_interface_skeleton_unexport_from_connection(
                    G_DBUS_INTERFACE_SKELETON(device->cradle_battery_iface),
                    connection);
            g_object_unref(device->cradle_battery_iface);
        }

        if (device->left_right_iface != NULL)
        {
            g_dbus_interface_skeleton_unexport_from_connection(
                    G_DBUS_INTERFACE_SKELETON(device->left_right_iface),
                    connection);
            g_object_unref(device->left_right_iface);
        }

        if (device->noise_cancelling_iface != NULL)
        {
            g_dbus_interface_skeleton_unexport_from_connection(
                    G_DBUS_INTERFACE_SKELETON(device->noise_cancelling_iface),
                    connection);
            g_object_unref(device->noise_cancelling_iface);
        }

        if (device->ambient_sound_mode_iface != NULL)
        {
            g_dbus_interface_skeleton_unexport_from_connection(
                    G_DBUS_INTERFACE_SKELETON(device->ambient_sound_mode_iface),
                    connection);
            g_object_unref(device->ambient_sound_mode_iface);
        }

        if (device->eq_iface != NULL)
        {
            g_dbus_interface_skeleton_unexport_from_connection(
                    G_DBUS_INTERFACE_SKELETON(device->eq_iface),
                    connection);
            g_object_unref(device->eq_iface);
        }

        if (device->auto_power_off_iface != NULL)
        {
            g_dbus_interface_skeleton_unexport_from_connection(
                    G_DBUS_INTERFACE_SKELETON(device->auto_power_off_iface),
                    connection);
            g_object_unref(device->auto_power_off_iface);
        }
    }
}

static gboolean device_source_prepare(GSource* source, gint* timeout)
{
    device_source_t* dev_source = (device_source_t*) source;

    if (dev_source->device->mdr_device == NULL)
    {
        return FALSE;
    }

    mdr_poll_info poll_info = mdr_device_poll_info(dev_source->device->mdr_device);
    *timeout = poll_info.timeout;
    dev_source->poll_fd.events
        = (dev_source->poll_fd.events & ~G_IO_OUT)
        | (poll_info.write ? G_IO_OUT : 0);
    dev_source->poll_fd.revents = 0;

    if (poll_info.timeout == 0)
    {
        return TRUE;
    }

    return FALSE;
}

static gboolean device_source_check(GSource* source)
{
    device_source_t* dev_source = (device_source_t*) source;

    return dev_source->poll_fd.revents != 0;
}

static gboolean device_source_dispatch(GSource* source,
                                       GSourceFunc callback,
                                       gpointer user_data)
{
    device_source_t* dev_source = (device_source_t*) source;

    mdr_device_t* mdr_device = dev_source->device->mdr_device;
    GPollFD* poll_fd = &dev_source->poll_fd;

    if (mdr_device == NULL)
    {
        return G_SOURCE_REMOVE;
    }

    // TOOD handle error
    if ((poll_fd->revents & G_IO_HUP) != 0)
    {
        g_warning("Lost connection to device '%s': %s",
                dev_source->device->dbus_name,
                poll_fd->revents & G_IO_ERR ? "ERR" : "HUP");
        device_remove(dev_source->device->dbus_name);
        return G_SOURCE_REMOVE;
    }

    mdr_device_process_by_availability(
            mdr_device,
            (poll_fd->revents & G_IO_IN) != 0,
            (poll_fd->revents & G_IO_OUT) != 0);

    if (callback != NULL)
    {
        callback(user_data);
    }

    return G_SOURCE_CONTINUE;
}

static void device_source_dispose(GSource* source)
{
    device_source_t* dev_source = (device_source_t*) source;

    device_unref(dev_source->device);
}
