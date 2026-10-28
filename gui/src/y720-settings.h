/* SPDX-License-Identifier: MIT */
#ifndef Y720_SETTINGS_H
#define Y720_SETTINGS_H

#include "y720-models.h"

G_BEGIN_DECLS

typedef struct {
    gboolean start_hidden;
    gboolean close_to_tray;
    gboolean global_hotkey;
    gboolean has_profile;
    Y720BacklightProfile profile;

    gchar *background_path;
    gdouble background_opacity;
    gdouble panel_opacity;
    gdouble window_opacity;
    gchar *accent;
    gchar *profile_hotkey;
    gchar *boost_hotkey;
    gboolean curve_enabled;
    gint curve_points[6];
} Y720Settings;

Y720Settings *y720_settings_new(void);
void y720_settings_free(Y720Settings *settings);
void y720_settings_save(const Y720Settings *settings);
void y720_settings_set_profile(Y720Settings *settings, const Y720BacklightProfile *profile);

gboolean y720_autostart_enabled(void);
gboolean y720_autostart_set(gboolean enabled, GError **error);
gchar *y720_autostart_path(void);

G_END_DECLS

#endif
