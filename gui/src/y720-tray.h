/* SPDX-License-Identifier: MIT */
#ifndef Y720_TRAY_H
#define Y720_TRAY_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _Y720Tray Y720Tray;
typedef void (*Y720TrayActionFunc)(const gchar *action, gpointer user_data);

Y720Tray *y720_tray_new(GDBusConnection *bus, Y720TrayActionFunc callback, gpointer user_data);
void y720_tray_free(Y720Tray *tray);
gboolean y720_tray_is_available(Y720Tray *tray);
void y720_tray_set_state(Y720Tray *tray, gboolean window_visible, gboolean controls_available,
                         const gchar *mode, gint rpm);
void y720_tray_set_hotkey_state(Y720Tray *tray, gboolean ok);
void y720_tray_set_profiles(Y720Tray *tray, const gchar *const *names, guint count, guint selected);
void y720_tray_set_curve_enabled(Y720Tray *tray, gboolean enabled);

G_END_DECLS

#endif
