/* SPDX-License-Identifier: MIT */
#ifndef Y720_PORTAL_HOTKEYS_H
#define Y720_PORTAL_HOTKEYS_H

#include "y720-hotkeys.h"

G_BEGIN_DECLS
#define Y720_TYPE_PORTAL_HOTKEYS (y720_portal_hotkeys_get_type())
G_DECLARE_FINAL_TYPE(Y720PortalHotkeys, y720_portal_hotkeys, Y720, PORTAL_HOTKEYS, GObject)

typedef struct {
    const gchar *id;             /* Stable across sessions, independent of array order. */
    const gchar *description;
    const gchar *accelerator;    /* GTK accelerator, converted to the XDG format. */
    Y720HotkeyFunc callback;     /* Called with the live owner, never a borrowed pointer. */
} Y720PortalBinding;
typedef void (*Y720HotkeyStateFunc)(GObject *owner);

Y720PortalHotkeys *y720_portal_hotkeys_new(const Y720PortalBinding *bindings, guint count,
                                         GObject *owner, Y720HotkeyStateFunc changed,
                                         GError **error);
void y720_portal_hotkeys_stop(Y720PortalHotkeys *self);
gboolean y720_portal_hotkeys_active(Y720PortalHotkeys *self);
gboolean y720_portal_hotkeys_pending(Y720PortalHotkeys *self);
const gchar *y720_portal_hotkeys_status(Y720PortalHotkeys *self);

/* Shared application backend: stop/record/quit use y720_hotkeys_stop(). */
gboolean y720_hotkeys_start_portal(const Y720PortalBinding *bindings, guint count,
                                  GObject *owner, Y720HotkeyStateFunc changed, GError **error);

/* Pure conversion, also used by protocol tests. X11 keycode:N is not portable. */
gchar *y720_portal_trigger(const gchar *accelerator, GError **error);
G_END_DECLS
#endif
