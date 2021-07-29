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

GDBusConnection* connection;
GMainLoop* loop;

static void
on_name_acquired(GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{}

static void
on_name_lost(GDBusConnection *connection,
             const gchar     *name,
             gpointer         user_data)
{
    g_message("Name not reserved, using %s",
              g_dbus_connection_get_unique_name(connection));
}

gint main_loop_poll(GPollFD *ufds,
                    guint nfsd,
                    gint timeout);

int main(int argc, char *argv[])
{
    GError* error = NULL;

    connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (connection == NULL)
    {
        g_prefix_error(&error, "Failed to connect to DBus.\n");
        g_error("%s\n", error->message);
        return -1;
    }

    g_bus_own_name_on_connection(
        connection,
        "org.mdr",
        G_BUS_NAME_OWNER_FLAGS_REPLACE,
        on_name_acquired,
        on_name_lost,
        NULL,
        NULL);

    devices_init();

    profile_init();
    profile_register();

    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    g_main_loop_unref(loop);

    g_dbus_connection_close_sync(connection, NULL, NULL);

    devices_deinit();

    return 0;
}

