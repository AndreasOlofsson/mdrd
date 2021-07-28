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

#ifndef __DEVICE_H__
#define __DEVICE_H__

#include <gio/gio.h>
#include <stdbool.h>
#include <poll.h>

typedef struct device device_t;

typedef void (*device_created_cb)(void* user_data);
typedef void (*device_create_error_cb)(void* user_data);

void devices_init();

void devices_deinit();

void device_add(const gchar* name,
                gint sock,
                device_created_cb,
                device_create_error_cb,
                void* user_data);

void device_remove(const gchar* name);

#endif /* __DEVICE_H__ */
