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

typedef struct device_source device_source_t;

extern GDBusConnection* connection;

GHashTable* device_table;

typedef struct device device_t;

typedef struct
{
    device_t* device;
    mdr_packet_battery_inquired_type_t type;
}
battery_update_callback_data_t;

struct device
{
    int refs;

    gchar* dbus_name;

    mdr_device_t* mdr_device;

    device_source_t* source;

    guint battery_timer;
    OrgMdrDevice* device_iface;
    OrgMdrBattery* battery_iface;
    OrgMdrLeftRightBattery* lr_battery_iface;
    OrgMdrCradleBattery* cradle_battery_iface;
    OrgMdrNoiseCancelling* nc_iface;
    OrgMdrAmbientSoundMode* asm_iface;
    OrgMdrEq* eq_iface;

    battery_update_callback_data_t* battery_cb_data;
    battery_update_callback_data_t* lr_battery_cb_data;
    battery_update_callback_data_t* cradle_battery_cb_data;

    mdr_packet_ncasm_inquired_type_t ncasm_type;
};

static void device_destroy(device_t* device);

static void device_ref(device_t* device)
{
    device->refs++;
}

static void device_unref(device_t* device)
{
    device->refs--;
    if (device->refs == 0)
    {
        device_destroy(device);
    }
}

struct device_source
{
    GSource source;
    device_t* device;
    GPollFD poll_fd;
};

static gboolean device_source_prepare(GSource*, gint* timeout);
static gboolean device_source_check(GSource*);
static gboolean device_source_dispatch(GSource*, GSourceFunc, gpointer);

static GSourceFuncs device_source_funcs = {
    .prepare = device_source_prepare,
    .check = device_source_check,
    .dispatch = device_source_dispatch,
};

static void device_removed(device_t*);
static gboolean device_update_batteries(void* user_data);
static void device_fetch_name(device_t* device);
static void device_ncasm_notify(void*, device_t* device);
static void device_eq_init(device_t* device);

static gboolean on_ncasm_disable(
        OrgMdrNoiseCancelling* profile_interface,
        GDBusMethodInvocation* invocation,
        device_t* device);

static gboolean on_ncasm_enable(
        OrgMdrNoiseCancelling* profile_interface,
        GDBusMethodInvocation* invocation,
        device_t* device);

static gboolean on_ncasm_set_asm_amount(
        OrgMdrNoiseCancelling* profile_interface,
        GDBusMethodInvocation* invocation,
        guint amount,
        device_t* device);

static gboolean on_ncasm_set_asm_mode(
        OrgMdrNoiseCancelling* profile_interface,
        GDBusMethodInvocation* invocation,
        const guchar* mode,
        device_t* device);

void devices_init()
{
    device_table = g_hash_table_new_full(
            g_str_hash,
            g_str_equal,
            g_free,
            (void (*)(void*)) device_removed);
}

void devices_deinit()
{
    g_hash_table_destroy(device_table);
}

typedef struct
{
    device_t* device;
    gchar* dbus_name;
    device_created_cb success_cb;
    device_create_error_cb error_cb;
    void* user_data;
}
device_finish_data_t;

static void device_add_finish(void* data, void* user_data);
static void device_add_error(void* user_data);

void device_add(const gchar* dbus_name,
                gint sock,
                device_created_cb success_cb,
                device_create_error_cb error_cb,
                void* user_data)
{
    device_finish_data_t* call_data = malloc(sizeof(device_finish_data_t));
    if (call_data == NULL)
    {
        error_cb(user_data);
        return;
    }

    device_t* device = malloc(sizeof(device_t));
    if (device == NULL)
    {
        free(call_data);
        error_cb(user_data);
        return;
    }
    device->refs = 1;

    device->dbus_name = g_strdup(dbus_name);
    device->battery_timer = -1;
    device->device_iface = NULL;
    device->battery_iface = NULL;
    device->lr_battery_iface = NULL;
    device->cradle_battery_iface = NULL;
    device->nc_iface = NULL;
    device->asm_iface = NULL;
    device->eq_iface = NULL;

    device->mdr_device = mdr_device_new_from_sock(sock);
    if (device->mdr_device == NULL)
    {
        free(call_data);
        free(device);
        error_cb(user_data);
        return;
    }

    device_source_t* dev_source =
        (device_source_t*) g_source_new(&device_source_funcs,
                                        sizeof(device_source_t));
    dev_source->poll_fd.fd = sock;
    dev_source->poll_fd.events = G_IO_IN | G_IO_OUT | G_IO_HUP | G_IO_ERR;
    dev_source->poll_fd.revents = 0;
    g_source_add_poll(&dev_source->source, &dev_source->poll_fd);

    dev_source->device = device;
    device->source = dev_source;

    g_source_attach(&dev_source->source, g_main_context_default());

    call_data->device = device;
    call_data->dbus_name = g_strdup(dbus_name);
    call_data->success_cb = success_cb;
    call_data->error_cb = error_cb;
    call_data->user_data = user_data;

    mdr_device_get_support_functions(
            device->mdr_device,
            device_add_finish,
            device_add_error,
            call_data);
}

static void device_add_finish(void* data, void* user_data)
{
    mdr_packet_connect_ret_support_function_t* supported_functions = data;
    device_finish_data_t* call_data = user_data;

    device_t* device = call_data->device;

    bool support_ncasm = mdr_packet_support_function_contains(
            supported_functions,
            MDR_PACKET_SUPPORT_FUNCTION_TYPE_NOISE_CANCELLING_AND_AMBIENT_SOUND_MODE);

    bool support_nc = support_ncasm
        || mdr_packet_support_function_contains(
            supported_functions,
            MDR_PACKET_SUPPORT_FUNCTION_TYPE_NOISE_CANCELLING);

    bool support_asm = support_ncasm
        || mdr_packet_support_function_contains(
            supported_functions,
            MDR_PACKET_SUPPORT_FUNCTION_TYPE_AMBIENT_SOUND_MODE);

    bool support_battery =
        mdr_packet_support_function_contains(
                supported_functions,
                MDR_PACKET_SUPPORT_FUNCTION_TYPE_BATTERY_LEVEL);

    bool support_lr_battery =
        mdr_packet_support_function_contains(
                supported_functions,
                MDR_PACKET_SUPPORT_FUNCTION_TYPE_LEFT_RIGHT_BATTERY_LEVEL);

    bool support_cradle_battery =
        mdr_packet_support_function_contains(
                supported_functions,
                MDR_PACKET_SUPPORT_FUNCTION_TYPE_CRADLE_BATTERY_LEVEL);

    bool support_eq =
        mdr_packet_support_function_contains(
                supported_functions,
                MDR_PACKET_SUPPORT_FUNCTION_TYPE_PRESET_EQ);

    free(supported_functions->function_types);
    free(supported_functions);

    device_fetch_name(device);

    if (support_eq)
    {
        device_eq_init(device);
    }

    g_message("Successfully connected to device '%s'. ", call_data->dbus_name);

    if (support_nc && support_asm)
    {
        device->ncasm_type = MDR_PACKET_NCASM_INQUIRED_TYPE_NOISE_CANCELLING_AND_ASM;
    }
    else if (support_nc)
    {
        device->ncasm_type = MDR_PACKET_NCASM_INQUIRED_TYPE_NOISE_CANCELLING;
    }
    else if (support_asm)
    {
        device->ncasm_type = MDR_PACKET_NCASM_INQUIRED_TYPE_ASM;
    }

    if (support_nc)
    {
        device->nc_iface = org_mdr_noise_cancelling_skeleton_new();

        GError* error = NULL;

        if (g_dbus_interface_skeleton_export(
                    G_DBUS_INTERFACE_SKELETON(device->nc_iface),
                    connection,
                    call_data->dbus_name,
                    &error))
        {
            g_signal_connect(
                    device->nc_iface,
                    "handle-disable",
                     G_CALLBACK(on_ncasm_disable),
                     device);

            g_signal_connect(
                    device->nc_iface,
                    "handle-enable",
                     G_CALLBACK(on_ncasm_enable),
                     device);

            g_debug("Registered noise cancelling interface. ");
        }
        else
        {
            g_error("Failed to register noise cancelling interface: "
                    "%s", error->message);
            device->nc_iface = NULL;
        }
    }

    if (support_asm)
    {
        device->asm_iface = org_mdr_ambient_sound_mode_skeleton_new();

        GError* error = NULL;

        if (g_dbus_interface_skeleton_export(
                    G_DBUS_INTERFACE_SKELETON(device->asm_iface),
                    connection,
                    call_data->dbus_name,
                    &error))
        {
            g_signal_connect(
                    device->asm_iface,
                    "handle-set-amount",
                     G_CALLBACK(on_ncasm_set_asm_amount),
                     device);

            g_signal_connect(
                    device->asm_iface,
                    "handle-set-mode",
                     G_CALLBACK(on_ncasm_set_asm_mode),
                     device);

            g_debug("Registered ambient sound mode interface. ");
        }
        else
        {
            g_error("Failed to register ambient sound mode interface: "
                    "%s", error->message);
            device->asm_iface = NULL;
        }
    }

    if (support_nc || support_asm)
    {
        mdr_device_ncasm_subscribe(
                device->mdr_device,
                device->ncasm_type,
                (void (*)(void*, void*)) device_ncasm_notify,
                device);

        mdr_device_get_ncasm_param(
                device->mdr_device,
                device->ncasm_type,
                (void (*)(void*, void*)) device_ncasm_notify,
                NULL,
                device);
    }

    if (support_battery)
    {
        device->battery_iface = org_mdr_battery_skeleton_new();

        GError* error = NULL;

        if (g_dbus_interface_skeleton_export(
                G_DBUS_INTERFACE_SKELETON(device->battery_iface),
                connection,
                call_data->dbus_name,
                &error))
        {
            device->battery_cb_data =
                g_malloc(sizeof(battery_update_callback_data_t));

            device->battery_cb_data->device = device;
            device->battery_cb_data->type =
                MDR_PACKET_BATTERY_INQUIRED_TYPE_BATTERY;

            g_debug("Registered battery interface. ");
        }
        else
        {
            g_error("Failed to register battery interface: "
                    "%s", error->message);
            device->battery_iface = NULL;
        }
    }

    if (support_lr_battery)
    {
        g_debug("Supports left-right battery.");

        device->lr_battery_iface = org_mdr_left_right_battery_skeleton_new();

        GError* error = NULL;

        if (g_dbus_interface_skeleton_export(
                G_DBUS_INTERFACE_SKELETON(device->lr_battery_iface),
                connection,
                call_data->dbus_name,
                &error))
        {
            device->lr_battery_cb_data =
                g_malloc(sizeof(battery_update_callback_data_t));

            device->lr_battery_cb_data->device = device;
            device->lr_battery_cb_data->type =
                MDR_PACKET_BATTERY_INQUIRED_TYPE_LEFT_RIGHT_BATTERY;

            g_debug("Registered left-right battery interface. ");
        }
        else
        {
            g_error("Failed to register left-right battery interface: "
                    "%s", error->message);
            device->lr_battery_iface = NULL;
        }
    }

    if (support_cradle_battery)
    {
        g_debug("Supports cradle battery.");

        device->cradle_battery_iface = org_mdr_cradle_battery_skeleton_new();

        GError* error = NULL;

        if (g_dbus_interface_skeleton_export(
                G_DBUS_INTERFACE_SKELETON(device->cradle_battery_iface),
                connection,
                call_data->dbus_name,
                &error))
        {
            device->cradle_battery_cb_data =
                g_malloc(sizeof(battery_update_callback_data_t));

            device->cradle_battery_cb_data->device = device;
            device->cradle_battery_cb_data->type =
                MDR_PACKET_BATTERY_INQUIRED_TYPE_CRADLE_BATTERY;

            g_debug("Registered cradle battery interface. ");
        }
        else
        {
            g_error("Failed to register cradle battery interface: "
                    "%s", error->message);
        }
    }

    if (support_battery || support_lr_battery || support_cradle_battery)
    {
        device_update_batteries(device);
        device->battery_timer = g_timeout_add(10000,
                                              device_update_batteries,
                                              device);
    }
    else
    {
        device->battery_timer = -1;
    }

    g_hash_table_insert(device_table, call_data->dbus_name, device);

    call_data->success_cb(call_data->user_data);
    free(call_data);
}

static void device_add_error(void* user_data)
{
    device_finish_data_t* call_data = user_data;

    g_warning("Failed to connect to device '%s' (%d). ",
              call_data->dbus_name,
              errno);

    g_free(call_data->dbus_name);

    device_unref(call_data->device);

    call_data->error_cb(call_data->user_data);
    free(call_data->device);
    free(call_data);
}

void device_remove(const gchar* device)
{
    g_hash_table_remove(device_table, device);
}

static void device_removed(device_t* device)
{
    if (device->battery_timer != -1)
    {
        g_source_remove(device->battery_timer);
        device->battery_timer = -1;
    }

    mdr_device_close(device->mdr_device);
    device->mdr_device = NULL;
    device_unref(device);
}

static void device_destroy(device_t* device)
{
    if (device->refs != 0)
    {
        g_error("device_destroy called with non-zero ref count");
        raise(SIGTRAP);
    }

    g_message("Device '%s' removed\n", device->dbus_name);
    g_free(device->dbus_name);

    g_source_destroy(&device->source->source);
    if (device->battery_timer != -1)
    {
        g_source_remove(device->battery_timer);
    }

    if (device->device_iface != NULL)
    {
        g_dbus_interface_skeleton_unexport_from_connection(
                G_DBUS_INTERFACE_SKELETON(device->device_iface),
                connection);
    }

    if (device->battery_iface != NULL)
    {
        g_dbus_interface_skeleton_unexport_from_connection(
                G_DBUS_INTERFACE_SKELETON(device->battery_iface),
                connection);
        g_free(device->battery_cb_data);
    }

    if (device->lr_battery_iface != NULL)
    {
        g_dbus_interface_skeleton_unexport_from_connection(
                G_DBUS_INTERFACE_SKELETON(device->lr_battery_iface),
                connection);
        g_free(device->lr_battery_cb_data);
    }

    if (device->cradle_battery_iface != NULL)
    {
        g_dbus_interface_skeleton_unexport_from_connection(
                G_DBUS_INTERFACE_SKELETON(device->cradle_battery_iface),
                connection);
        g_free(device->cradle_battery_cb_data);
    }

    if (device->nc_iface != NULL)
    {
        g_dbus_interface_skeleton_unexport_from_connection(
                G_DBUS_INTERFACE_SKELETON(device->nc_iface),
                connection);
    }

    if (device->asm_iface != NULL)
    {
        g_dbus_interface_skeleton_unexport_from_connection(
                G_DBUS_INTERFACE_SKELETON(device->asm_iface),
                connection);
    }

    free(device);
}

static gboolean device_source_prepare(GSource* source, gint* timeout)
{
    device_source_t* dev_source = (device_source_t*) source;

    if (dev_source->device->mdr_device == NULL)
    {
        return FALSE;
    }

    *timeout = mdr_device_wait_timeout(dev_source->device->mdr_device);
    if (*timeout == 0)
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
                                       GSourceFunc cb,
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
    if ((poll_fd->revents & (G_IO_ERR | G_IO_HUP)) != 0)
    {
        g_warning("Lost connection to device '%s'",
                  dev_source->device->dbus_name);
        device_remove(dev_source->device->dbus_name);
        return G_SOURCE_REMOVE;
    }

    mdr_device_process_by_availability(
            mdr_device,
            (poll_fd->revents & G_IO_IN) != 0,
            (poll_fd->revents & G_IO_OUT) != 0);

    poll_fd->events =
            (mdr_device_waiting_read(mdr_device) ? G_IO_IN : 0)
         | (mdr_device_waiting_write(mdr_device) ? G_IO_OUT : 0)
         | G_IO_HUP | G_IO_ERR;
    dev_source->poll_fd.revents = 0;

    if (cb != NULL)
    {
        cb(user_data);
    }

    return G_SOURCE_CONTINUE;
}

static void device_fetch_name_result(void*, void*);
static void device_fetch_name_error(void*);

static void device_fetch_name(device_t* device)
{
    device_ref(device);
    mdr_device_get_device_info(
            device->mdr_device,
            MDR_PACKET_DEVICE_INFO_INQUIRED_TYPE_MODEL_NAME,
            device_fetch_name_result,
            device_fetch_name_error,
            device);
}

static void device_fetch_name_result(void* data, void* user_data)
{
    mdr_packet_device_info_string_t* name = data;
    device_t* device = user_data;

    device->device_iface = org_mdr_device_skeleton_new();

    GError* error = NULL;

    if (g_dbus_interface_skeleton_export(
            G_DBUS_INTERFACE_SKELETON(device->device_iface),
            connection,
            device->dbus_name,
            &error))
    {
        org_mdr_device_set_name(device->device_iface,
                g_strndup((gchar*) name->string, name->len));

        g_debug("Registered device interface. ");
    }
    else
    {
        g_warning("Failed to register device interface: "
                  "%s", error->message);
    }

    free(name->string);
    free(name);

    device_unref(device);
}

static void device_fetch_name_error(void* user_data)
{
    device_t* device = user_data;

    g_warning("Failed to fetch device name (%d). ", errno);
    
    device_unref(device);
}

static const gchar* get_preset_name(mdr_packet_eqebb_eq_preset_id_t);
static mdr_packet_eqebb_eq_preset_id_t get_preset_id(const gchar* name);

static void device_eq_cap_result(mdr_packet_eqebb_capability_eq_t*, device_t*);
static void device_eq_cap_error(device_t*);

static void device_eq_init(device_t* device)
{
    device_ref(device);
    mdr_device_get_eqebb_capability(
            device->mdr_device,
            MDR_PACKET_EQEBB_INQUIRED_TYPE_PRESET_EQ,
            MDR_PACKET_EQEBB_DISPLAY_LANGUAGE_UNDEFINED_LANGUAGE,
            (void (*)(void*, void*)) device_eq_cap_result,
            (void (*)(void*)) device_eq_cap_error,
            device);
}

static void device_eq_cap_error(device_t* device)
{
    g_warning("Failed to get EQ presets for device '%s'.", device->dbus_name);
    device_unref(device);
}

static void device_eq_param_result(mdr_packet_eqebb_param_eq_t*, device_t*);
static void device_eq_param_error(device_t*);

static void device_eq_cap_result(mdr_packet_eqebb_capability_eq_t* caps,
                                 device_t* device)
{
    device->eq_iface = org_mdr_eq_skeleton_new();

    org_mdr_eq_set_band_count(device->eq_iface, caps->band_count);
    org_mdr_eq_set_level_steps(device->eq_iface, caps->level_steps);

    const gchar** presets = g_new(const gchar*, caps->num_presets + 1);
    int j = 0;
    for (int i = 0; i < caps->num_presets; i++)
    {
        const gchar* name = get_preset_name(caps->presets[i].preset_id);

        if (name != NULL)
        {
            presets[j] = g_strdup(name);
            j++;
        }

        free(caps->presets[i].name);
    }
    presets[j] = NULL;
    org_mdr_eq_set_available_presets(device->eq_iface, presets);

    free(caps->presets);
    free(caps);

    mdr_device_get_eqebb_param(
            device->mdr_device,
            MDR_PACKET_EQEBB_INQUIRED_TYPE_PRESET_EQ,
            (void (*)(void*, void*)) device_eq_param_result,
            (void (*)(void*)) device_eq_param_error,
            device);
}

static gboolean on_eq_set_preset(
        OrgMdrNoiseCancelling* profile_interface,
        GDBusMethodInvocation* invocation,
        const gchar* preset,
        device_t* device);

static gboolean on_eq_set_levels(
        OrgMdrNoiseCancelling* profile_interface,
        GDBusMethodInvocation* invocation,
        GVariant* levels,
        device_t* device);

static void device_eq_param_result(mdr_packet_eqebb_param_eq_t* eq_param,
                                   device_t* device)
{
    const gchar* preset_name = get_preset_name(eq_param->preset_id);

    if (preset_name != NULL)
    {
        org_mdr_eq_set_preset(device->eq_iface, g_strdup(preset_name));
    }

    GVariantBuilder* levels = g_variant_builder_new(G_VARIANT_TYPE("au"));

    for (int i = 0; i < eq_param->num_levels; i++)
    {
        g_variant_builder_add(levels, "u", (guint32) eq_param->levels[i]);
    }

    org_mdr_eq_set_levels(device->eq_iface, g_variant_builder_end(levels));

    GError* error = NULL;

    if (g_dbus_interface_skeleton_export(
            G_DBUS_INTERFACE_SKELETON(device->eq_iface),
            connection,
            device->dbus_name,
            &error))
    {
        g_signal_connect(
                device->eq_iface,
                "handle-set-preset",
                 G_CALLBACK(on_eq_set_preset),
                 device);

        g_signal_connect(
                device->eq_iface,
                "handle-set-levels",
                 G_CALLBACK(on_eq_set_levels),
                 device);

        g_debug("Registered EQ interface. ");
    }
    else
    {
        g_warning("Failed to register EQ interface: "
                  "%s", error->message);
    }

    device_unref(device);
}

static void device_eq_param_error(device_t* device)
{
    g_warning("Failed to get EQ param for device '%s'.", device->dbus_name);

    // TODO destroy interface

    device_unref(device);
}

static void on_eq_set_preset_success(void*, GDBusMethodInvocation* invocation);
static void on_eq_set_preset_error(GDBusMethodInvocation* invocation);

static gboolean on_eq_set_preset(
        OrgMdrNoiseCancelling* profile_interface,
        GDBusMethodInvocation* invocation,
        const gchar* preset_name,
        device_t* device)
{
    mdr_packet_eqebb_eq_preset_id_t preset = get_preset_id(preset_name);
    
    if (preset == MDR_PACKET_EQEBB_EQ_PRESET_ID_UNSPECIFIED)
    {
        g_dbus_method_invocation_return_dbus_error(invocation,
                "org.mdr.UnknownPreset",
                "Unknown EQ preset. ");
    }
    else
    {
        mdr_device_set_eq_preset(device->mdr_device,
                preset,
                (void (*)(void*, void*)) on_eq_set_preset_success,
                (void (*)(void*)) on_eq_set_preset_error,
                invocation);
    }

    return TRUE;
}

static void on_eq_set_preset_success(void* _, GDBusMethodInvocation* invocation)
{
    g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
}

static void on_eq_set_preset_error(GDBusMethodInvocation* invocation)
{
    g_dbus_method_invocation_return_dbus_error(invocation,
            "org.mdr.Failed",
            "Failed to set EQ preset. ");
}

static void on_eq_set_levels_success(void*, GDBusMethodInvocation* invocation);
static void on_eq_set_levels_error(GDBusMethodInvocation* invocation);

static gboolean on_eq_set_levels(
        OrgMdrNoiseCancelling* profile_interface,
        GDBusMethodInvocation* invocation,
        GVariant* levels,
        device_t* device)
{
    gsize num_levels = g_variant_n_children(levels);

    if (num_levels != org_mdr_eq_get_band_count(device->eq_iface))
    {
        g_dbus_method_invocation_return_dbus_error(invocation,
                "org.mdr.BadBandCount",
                "Bad EQ band count. ");

        return TRUE;
    }

    guint32 level_max = org_mdr_eq_get_level_steps(device->eq_iface);

    uint8_t* mdr_levels = g_new(uint8_t, num_levels);
    for (int i = 0; i < num_levels; i++)
    {
        guint32 level;
        g_variant_get_child(levels, i, "u", &level); 

        if (level > level_max)
        {
            free(mdr_levels);

            g_dbus_method_invocation_return_dbus_error(invocation,
                    "org.mdr.InvalidLevel",
                    "Invalid EQ level. ");

            return TRUE;
        }

        mdr_levels[i] = level;
    }

    mdr_device_set_eq_levels(device->mdr_device,
            num_levels,
            mdr_levels,
            (void (*)(void*, void*)) on_eq_set_levels_success,
            (void (*)(void*)) on_eq_set_levels_error,
            invocation);

    g_free(mdr_levels);

    return TRUE;
}

static void on_eq_set_levels_success(void* _, GDBusMethodInvocation* invocation)
{
    g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
}

static void on_eq_set_levels_error(GDBusMethodInvocation* invocation)
{
    g_dbus_method_invocation_return_dbus_error(invocation,
            "org.mdr.Failed",
            "Failed to set EQ levels. ");
}

static const gchar* get_preset_name(mdr_packet_eqebb_eq_preset_id_t preset)
{
    switch (preset)
    {
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_OFF:
            return "Off"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_ROCK:
            return "Rock"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_POP:
            return "Pop"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_JAZZ:
            return "Jazz"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_DANCE:
            return "Dance"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_EDM:
            return "EDM"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_R_AND_B_HIP_HOP:
            return "R6B Hip Hop"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_ACOUSTIC:
            return "Acoustic"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_BRIGHT:
            return "Bright"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_EXCITED:
            return "Excited"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_MELLOW:
            return "Mellow"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_RELAXED:
            return "Relaxed"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_VOCAL:
            return "Vocal"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_TREBLE:
            return "Treble"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_BASS:
            return "Bass"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_SPEECH:
            return "Speech"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_CUSTOM:
            return "Custom"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_USER_SETTING1:
            return "User Setting 1"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_USER_SETTING2:
            return "User Setting 2"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_USER_SETTING3:
            return "User Setting 3"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_USER_SETTING4:
            return "User Setting 4"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_USER_SETTING5:
            return "User Setting 5"; break;
        case MDR_PACKET_EQEBB_EQ_PRESET_ID_UNSPECIFIED:
            return "Unspecified"; break;
        default:
            return NULL;
    }
}

static mdr_packet_eqebb_eq_preset_id_t get_preset_id(const gchar* name)
{
    if (g_str_equal(name, "Off"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_OFF;
    else if (g_str_equal(name, "Rock"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_ROCK;
    else if (g_str_equal(name, "Pop"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_POP;
    else if (g_str_equal(name, "Jazz"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_JAZZ;
    else if (g_str_equal(name, "Dance"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_DANCE;
    else if (g_str_equal(name, "EDM"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_EDM;
    else if (g_str_equal(name, "R6B Hip Hop"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_R_AND_B_HIP_HOP;
    else if (g_str_equal(name, "Acoustic"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_ACOUSTIC;
    else if (g_str_equal(name, "Bright"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_BRIGHT;
    else if (g_str_equal(name, "Excited"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_EXCITED;
    else if (g_str_equal(name, "Mellow"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_MELLOW;
    else if (g_str_equal(name, "Relaxed"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_RELAXED;
    else if (g_str_equal(name, "Vocal"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_VOCAL;
    else if (g_str_equal(name, "Treble"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_TREBLE;
    else if (g_str_equal(name, "Bass"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_BASS;
    else if (g_str_equal(name, "Speech"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_SPEECH;
    else if (g_str_equal(name, "Custom"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_CUSTOM;
    else if (g_str_equal(name, "User Setting 1"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_USER_SETTING1;
    else if (g_str_equal(name, "User Setting 2"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_USER_SETTING2;
    else if (g_str_equal(name, "User Setting 3"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_USER_SETTING3;
    else if (g_str_equal(name, "User Setting 4"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_USER_SETTING4;
    else if (g_str_equal(name, "User Setting 5"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_USER_SETTING5;
    else if (g_str_equal(name, "Unspecified"))
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_UNSPECIFIED;
    else
        return MDR_PACKET_EQEBB_EQ_PRESET_ID_UNSPECIFIED;
}

static void device_update_batteries_success(void* result, void* user_data);
static void device_update_batteries_error(void* user_data);

static gboolean device_update_batteries(void* user_data)
{
    device_t* device = user_data;

    if (device->battery_iface != NULL)
    {
        device_ref(device);
        mdr_device_get_battery_level(
                device->mdr_device,
                MDR_PACKET_BATTERY_INQUIRED_TYPE_BATTERY,
                device_update_batteries_success,
                device_update_batteries_error,
                device->battery_cb_data);
    }

    if (device->lr_battery_iface != NULL)
    {
        device_ref(device);
        mdr_device_get_battery_level(
                device->mdr_device,
                MDR_PACKET_BATTERY_INQUIRED_TYPE_LEFT_RIGHT_BATTERY,
                device_update_batteries_success,
                device_update_batteries_error,
                device->lr_battery_cb_data);
    }

    if (device->cradle_battery_iface != NULL)
    {
        device_ref(device);
        mdr_device_get_battery_level(
                device->mdr_device,
                MDR_PACKET_BATTERY_INQUIRED_TYPE_CRADLE_BATTERY,
                device_update_batteries_success,
                device_update_batteries_error,
                device->cradle_battery_cb_data);
    }

    return TRUE;
}

static void device_update_batteries_success(void* result, void* user_data)
{
    battery_update_callback_data_t* callback_data = user_data;
    device_t* device = callback_data->device;

    switch (callback_data->type)
    {
        case MDR_PACKET_BATTERY_INQUIRED_TYPE_BATTERY:
        {
            mdr_packet_battery_status_t* battery_status = result;

            org_mdr_battery_set_level(
                    device->battery_iface,
                    battery_status->level);
            org_mdr_battery_set_charging(
                    device->battery_iface,
                    battery_status->charging != 0);

            free(battery_status);
        }
        break;

        case MDR_PACKET_BATTERY_INQUIRED_TYPE_LEFT_RIGHT_BATTERY:
        {
            mdr_packet_battery_status_left_right_t* battery_status = result;

            org_mdr_left_right_battery_set_left_level(
                    device->lr_battery_iface,
                    battery_status->left.level);
            org_mdr_left_right_battery_set_left_charging(
                    device->lr_battery_iface,
                    battery_status->left.charging != 0);
            org_mdr_left_right_battery_set_right_level(
                    device->lr_battery_iface,
                    battery_status->right.level);
            org_mdr_left_right_battery_set_right_charging(
                    device->lr_battery_iface,
                    battery_status->right.charging != 0);

            free(battery_status);
        }
        break;

        case MDR_PACKET_BATTERY_INQUIRED_TYPE_CRADLE_BATTERY:
        {
            mdr_packet_battery_status_t* battery_status = result;

            org_mdr_cradle_battery_set_level(
                    device->cradle_battery_iface,
                    battery_status->level);
            org_mdr_cradle_battery_set_charging(
                    device->cradle_battery_iface,
                    battery_status->charging != 0);

            free(battery_status);
        }
        break;
    }

    device_unref(device);
}

static void device_update_batteries_error(void* user_data)
{
    battery_update_callback_data_t* callback_data = user_data;
    device_t* device = callback_data->device;

    g_warning("Failed to update batteries for device '%s'\n", device->dbus_name);

    device_unref(device);
}

static void device_ncasm_notify(void* data, device_t* device)
{
    g_debug("Got NCASM data");

    switch (device->ncasm_type)
    {
        case MDR_PACKET_NCASM_INQUIRED_TYPE_NOISE_CANCELLING:
        {
            mdr_packet_ncasm_param_noise_cancelling_t* param = data;

            if (param->nc_setting_type ==
                    MDR_PACKET_NCASM_NC_SETTING_TYPE_ON_OFF)
            {
                if (device->nc_iface != NULL)
                {
                    org_mdr_noise_cancelling_set_enabled(
                            device->nc_iface,
                            param->nc_setting_value ==
                                MDR_PACKET_NCASM_NC_SETTING_VALUE_ON);
                }
            }

            free(param);
        }
        break;

        case MDR_PACKET_NCASM_INQUIRED_TYPE_ASM:
        {
            mdr_packet_ncasm_param_asm_t* param = data;

            if (device->asm_iface != NULL)
            {
                org_mdr_ambient_sound_mode_set_mode(
                        device->asm_iface,
                        param->asm_id == MDR_PACKET_NCASM_ASM_ID_VOICE
                            ? "voice"
                            : "normal");

                org_mdr_ambient_sound_mode_set_amount(
                        device->asm_iface,
                        param->asm_amount);
            }

            free(param);
        }
        break;

        case MDR_PACKET_NCASM_INQUIRED_TYPE_NOISE_CANCELLING_AND_ASM:
        {
            mdr_packet_ncasm_param_noise_cancelling_asm_t* param = data;

            if (device->nc_iface != NULL)
            {
                org_mdr_noise_cancelling_set_enabled(
                        device->nc_iface,
                        param->ncasm_effect !=
                            MDR_PACKET_NCASM_NCASM_EFFECT_OFF);
            }

            if (device->asm_iface != NULL)
            {
                org_mdr_ambient_sound_mode_set_mode(
                        device->asm_iface,
                        param->asm_id == MDR_PACKET_NCASM_ASM_ID_VOICE
                            ? "voice"
                            : "normal");

                org_mdr_ambient_sound_mode_set_amount(
                        device->asm_iface,
                        param->asm_amount);
            }

            free(param);
        }
        break;
    }
}

static gboolean on_ncasm_disable(
        OrgMdrNoiseCancelling* profile_interface,
        GDBusMethodInvocation* invocation,
        device_t* device)
{
    mdr_device_ncasm_disable(
            device->mdr_device,
            device->ncasm_type,
            NULL,
            NULL,
            NULL);

    g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));

    return TRUE;
}

static gboolean on_ncasm_enable(
        OrgMdrNoiseCancelling* profile_interface,
        GDBusMethodInvocation* invocation,
        device_t* device)
{
    mdr_device_ncasm_enable_nc(
            device->mdr_device,
            device->ncasm_type,
            NULL,
            NULL,
            NULL);

    g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));

    return TRUE;
}

static gboolean on_ncasm_set_asm_amount(
        OrgMdrNoiseCancelling* profile_interface,
        GDBusMethodInvocation* invocation,
        guint amount,
        device_t* device)
{
    bool voice = g_str_equal(
            org_mdr_ambient_sound_mode_get_mode(device->asm_iface),
            "voice");

    mdr_device_ncasm_enable_asm(
            device->mdr_device,
            device->ncasm_type,
            voice,
            amount,
            NULL,
            NULL,
            NULL);

    g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));

    return TRUE;
}

static gboolean on_ncasm_set_asm_mode(
        OrgMdrNoiseCancelling* profile_interface,
        GDBusMethodInvocation* invocation,
        const guchar* mode,
        device_t* device)
{
    bool voice;

    if (g_str_equal(mode, "voice"))
    {
        voice = true;
    }
    else if (g_str_equal(mode, "normal"))
    {
        voice = false;
    }
    else
    {
        g_dbus_method_invocation_return_dbus_error(
                invocation,
                "org.mdr.InvalidValue",
                "mode must be 'voice' or 'normal'");
        return TRUE;
    }

    mdr_device_ncasm_enable_asm(
            device->mdr_device,
            device->ncasm_type,
            voice,
            org_mdr_ambient_sound_mode_get_amount(device->asm_iface),
            NULL,
            NULL,
            NULL);

    g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));

    return TRUE;
}
