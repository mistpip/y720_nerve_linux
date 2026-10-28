/* SPDX-License-Identifier: MIT */
#ifndef Y720_WINDOW_H
#define Y720_WINDOW_H

#include <adwaita.h>
#include "y720-settings.h"

G_BEGIN_DECLS

#define Y720_TYPE_WINDOW (y720_window_get_type())
G_DECLARE_FINAL_TYPE(Y720Window, y720_window, Y720, WINDOW, AdwApplicationWindow)

Y720Window *y720_window_new(AdwApplication *application, Y720Settings *settings,
                            gboolean tray_available, gboolean hotkey_ok, gboolean x11_session);

void y720_window_start(Y720Window *window);
void y720_window_show_page(Y720Window *window, guint page);
void y720_window_refresh(Y720Window *window);
void y720_window_set_fan_mode(Y720Window *window, const gchar *mode);
void y720_window_toggle_boost(Y720Window *window);
void y720_window_set_curve_enabled(Y720Window *window, gboolean enabled);
void y720_window_curve_toggle(Y720Window *window);
gboolean y720_window_curve_active(Y720Window *window);
void y720_window_cycle_profile(Y720Window *window);
guint y720_window_profile_count(Y720Window *window);
const gchar *y720_window_profile_name(Y720Window *window, guint index);
guint y720_window_profile_selected(Y720Window *window);
void y720_window_select_profile(Y720Window *window, guint index);
void y720_window_show_settings(Y720Window *window);
void y720_window_show_about(Y720Window *window);
void y720_window_show_toast(Y720Window *window, const gchar *message);
gboolean y720_window_fan_controls(Y720Window *window);
const gchar *y720_window_fan_mode(Y720Window *window);
gint y720_window_fan_rpm(Y720Window *window);
void y720_window_set_quitting(Y720Window *window);
GtkWidget *y720_window_dialog(Y720Window *window);
void y720_window_apply_appearance(Y720Window *window);

G_END_DECLS

#endif
