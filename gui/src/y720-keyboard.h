/* SPDX-License-Identifier: MIT */
#ifndef Y720_KEYBOARD_H
#define Y720_KEYBOARD_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

GtkWidget *y720_keyboard_new(void);
void y720_keyboard_set_profile(GtkWidget *keyboard, const gint colors[4],
                               const gint brightness[4], gint selected);
void y720_keyboard_set_selected(GtkWidget *keyboard, gint selected);
gint y720_keyboard_get_selected(GtkWidget *keyboard);
typedef void (*Y720KeyboardCallback)(gint segment, gpointer user_data);
void y720_keyboard_set_callback(GtkWidget *keyboard, Y720KeyboardCallback callback, gpointer user_data);

G_END_DECLS

#endif
