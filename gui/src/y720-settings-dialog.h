/* SPDX-License-Identifier: MIT */
#ifndef Y720_SETTINGS_DIALOG_H
#define Y720_SETTINGS_DIALOG_H

#include <adwaita.h>
#include "y720-settings.h"

G_BEGIN_DECLS

typedef void (*Y720AppearanceChangedFunc)(gpointer user_data);
typedef void (*Y720ShortcutChangedFunc)(gpointer user_data);
typedef void (*Y720CurveChangedFunc)(gpointer user_data);

AdwDialog *y720_settings_dialog_show(GtkWindow *parent, Y720Settings *settings,
                               gboolean tray_available, gboolean hotkey_ok,
                               gboolean x11_session,
                               Y720AppearanceChangedFunc changed,
                               gpointer changed_data,
                               Y720ShortcutChangedFunc shortcut_changed,
                               gpointer shortcut_data,
                               Y720CurveChangedFunc curve_changed,
                               gpointer curve_data);

G_END_DECLS

#endif
