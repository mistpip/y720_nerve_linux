/* SPDX-License-Identifier: MIT */
#ifndef Y720_HOTKEYS_H
#define Y720_HOTKEYS_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef void (*Y720HotkeyFunc)(gpointer user_data);

typedef struct {
    const gchar *accelerator;
    Y720HotkeyFunc callback;
    gpointer user_data;
} Y720HotkeyBinding;

gboolean y720_hotkeys_start(const Y720HotkeyBinding *bindings, guint count,
                            GError **error);
void y720_hotkeys_stop(void);
gboolean y720_hotkeys_active(void);

G_END_DECLS

#endif
