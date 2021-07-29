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
#include "generated/mdr_device.h"

#include <signal.h>

extern GDBusConnection* connection;

typedef struct device_source device_source_t;

struct device
{
    int ref_count;
    const gchar* dbus_name;
    mdr_device_t* mdr_device;

    device_source_t* source;

    OrgMdrDevice* device_iface;
    OrgMdrBattery* battery_iface;
    OrgMdrLeftRightBattery* left_right_battery_iface;
    OrgMdrCradleBattery* cradle_battery_iface;
    OrgMdrLeftRight* left_right_iface;
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

    device->battery_iface = NULL;
    device->left_right_battery_iface = NULL;
    device->cradle_battery_iface = NULL;
    device->left_right_iface = NULL;

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

static void device_init_battery(device_t*);
static void device_init_left_right_battery(device_t*);
static void device_init_cradle_battery(device_t*);
static void device_init_left_right_connection_status(device_t*);

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

    if (supported_functions.battery)
        device_init_battery(device);

    if (supported_functions.left_right_battery)
        device_init_left_right_battery(device);

    if (supported_functions.left_right_connection_status)
        device_init_left_right_connection_status(device);

    if (supported_functions.cradle_battery)
        device_init_cradle_battery(device);
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
        device_ref(device);
    }
}

static void device_init_battery_error(void* user_data)
{
    device_t* device = user_data;

    g_warning("Device init battery failed: %d", errno);
    
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
        device_ref(device);
    }
}

static void device_init_left_right_battery_error(void* user_data)
{
    device_t* device = user_data;

    g_warning("Device init left-right battery failed: %d", errno);
    
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
        device_ref(device);
    }
}

static void device_init_cradle_battery_error(void* user_data)
{
    device_t* device = user_data;

    g_warning("Device init cradle battery failed: %d", errno);
    
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
        device_ref(device);
    }
}

static void device_init_left_right_connection_status_error(void* user_data)
{
    device_t* device = user_data;

    g_warning("Device init left-right connection status failed: %d", errno);
    
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


static void device_add_init_error(void* user_data)
{
    device_add_init_data* init_data = user_data;

    init_data->error_cb(init_data->error_cb);
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
        org_mdr_device_set_name(device->device_iface, "");
        g_dbus_interface_skeleton_flush(
                G_DBUS_INTERFACE_SKELETON(device->device_iface));

        g_dbus_interface_skeleton_unexport_from_connection(
                G_DBUS_INTERFACE_SKELETON(device->device_iface),
                connection);
        g_object_unref(device->device_iface);

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
        = dev_source->poll_fd.events & ~G_IO_OUT
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
