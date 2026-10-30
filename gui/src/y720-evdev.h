/* SPDX-License-Identifier: MIT */
#ifndef Y720_EVDEV_H
#define Y720_EVDEV_H

#include <gio/gio.h>
#include "y720-hotkeys.h"

G_BEGIN_DECLS

gboolean y720_evdev_start(const Y720HotkeyBinding *bindings, guint count, GError **error);
void y720_evdev_stop(void);
gboolean y720_evdev_active(void);

typedef void (*Y720EvdevCaptureFunc)(guint code, gpointer user_data);
gboolean y720_evdev_capture_start(Y720EvdevCaptureFunc callback, gpointer user_data,
                                  GError **error);
void y720_evdev_capture_stop(void);

G_END_DECLS

#endif
