/* SPDX-License-Identifier: MIT */
#ifndef Y720_APP_H
#define Y720_APP_H

#include <adwaita.h>

G_BEGIN_DECLS

#define Y720_TYPE_APP (y720_app_get_type())
G_DECLARE_FINAL_TYPE(Y720App, y720_app, Y720, APP, AdwApplication)

Y720App *y720_app_new(gboolean hidden, const gchar *screenshot_path,
                      const gchar *screenshot_page);

void y720_app_reload_shortcut(Y720App *app);

G_END_DECLS

#endif
