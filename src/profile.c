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

#include "profile.h"

#include "device.h"

#include "bluez_profile.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <stdio.h>

extern GDBusConnection* connection;

static gboolean on_profile_new_connection(
        OrgBluezProfile1* profile_interface,
        GDBusMethodInvocation* invocation,
        GUnixFDList* fds,
        const gchar* device,
        GVariant* fd_ref,
        GVariant* fd_propertes,
        gpointer user_data);

static gboolean on_profile_request_disconnection(
        OrgBluezProfile1* profile_interface,
        GDBusMethodInvocation* invocation,
        const gchar* device,
        gpointer user_data);

static gboolean on_profile_release(
        OrgBluezProfile1* profile_interface,
        GDBusMethodInvocation* invocation,
        gpointer user_data);

static OrgBluezProfile1* profile_interface;

void profile_init()
{
    GError* error = NULL;

    profile_interface = org_bluez_profile1_skeleton_new();

    g_signal_connect(profile_interface,
                     "handle-new-connection",
                     G_CALLBACK(on_profile_new_connection),
                     NULL);

    g_signal_connect(profile_interface,
                     "handle-request-disconnection",
                     G_CALLBACK(on_profile_request_disconnection),
                     NULL);

    g_signal_connect(profile_interface,
                     "handle-release",
                     G_CALLBACK(on_profile_release),
                     NULL);

    if (!g_dbus_interface_skeleton_export(
            G_DBUS_INTERFACE_SKELETON(profile_interface),
            connection,
            "/org/mdr",
            &error))
    {
        g_warning("Failed to register profile: %s\n", error->message);
        exit(1);
    }
}

void profile_deinit()
{
    g_dbus_interface_skeleton_unexport(
            G_DBUS_INTERFACE_SKELETON(profile_interface));

    g_object_unref(profile_interface);
}

static void on_profile_new_connection_success(void* user_data);
static void on_profile_new_connection_error(void* user_data);

static gboolean on_profile_new_connection(
        OrgBluezProfile1* profile_interface,
        GDBusMethodInvocation* invocation,
        GUnixFDList* fds,
        const gchar* device,
        GVariant* fd_ref,
        GVariant* fd_propertes,
        gpointer user_data)
{
    if (fds == NULL)
    {
        g_warning("Connection to '%s' requested without a handle.",
                  device);

        g_dbus_method_invocation_return_dbus_error(
                invocation,
                "org.bluez.Error.Rejected",
                "No FD supplied.");

        return TRUE;
    }

    gint fd = g_unix_fd_list_get(fds, g_variant_get_handle(fd_ref), NULL);

    g_message("Connecting to new device '%s'",
            device);

    device_add(device,
               fd,
               on_profile_new_connection_success,
               on_profile_new_connection_error,
               invocation);

    return TRUE;
}

static void on_profile_new_connection_success(void* user_data)
{
    GDBusMethodInvocation* invocation = user_data;

    g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
}

static void on_profile_new_connection_error(void* user_data)
{
    GDBusMethodInvocation* invocation = user_data;

    g_dbus_method_invocation_return_dbus_error(
            invocation,
            "org.bluez.Error.Rejected",
            "Failed to add device.");
}

static gboolean on_profile_request_disconnection(
        OrgBluezProfile1* profile_interface,
        GDBusMethodInvocation* invocation,
        const gchar* device,
        gpointer user_data)
{
    g_message("on request disconnect %s %ld\n", device, (size_t) user_data);

    device_remove(device);

    return TRUE;
}

static gboolean on_profile_release(OrgBluezProfile1* profile_interface,
                                   GDBusMethodInvocation* invocation,
                                   gpointer user_data)
{
    g_message("on release %ld\n", (size_t) user_data);

    g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));

    profile_deinit();

    extern GMainLoop* loop;
    g_main_loop_quit(loop);

    return TRUE;
}

void profile_register()
{
    GError* error = NULL;

    GVariantBuilder* options = g_variant_builder_new(G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(options, "{sv}",
                          "Name",
                          g_variant_new_string("MDR"));
    g_variant_builder_add(options, "{sv}",
                          "Role",
                          g_variant_new_string("client"));
    g_variant_builder_add(options, "{sv}",
                          "AutoConnect",
                          g_variant_new_boolean(TRUE));

    GVariant* result = g_dbus_connection_call_sync(
            connection,
            "org.bluez",
            "/org/bluez",
            "org.bluez.ProfileManager1",
            "RegisterProfile",
            g_variant_new("(osa{sv})",
                "/org/mdr",
                "96CC203E-5068-46AD-B32D-E316F5E069BA",
                options
            ),
            NULL,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            NULL,
            &error);

    if (result == NULL)
    {
        g_prefix_error(&error, "Failed to register MDR profile\n");
        g_error("%s\n", error->message);
        g_abort();
    }

    g_variant_builder_unref(options);
    g_variant_unref(result);
}

