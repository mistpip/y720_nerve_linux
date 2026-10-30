/* SPDX-License-Identifier: MIT */
#include <gtk/gtk.h>
#include <string.h>

#include "y720-app.h"
#include "y720-settings.h"
#include "y720-i18n.h"

int main(int argc, char **argv)
{
    /* GTK's own buttons/dialogs choose translations during initialization.
     * Load preferences without touching hardware, before creating the app. */
    Y720Settings *preferences = y720_settings_new();
    y720_i18n_prepare_environment(preferences->language);
    y720_settings_free(preferences);
    gboolean hidden = FALSE;
    const gchar *screenshot = NULL;
    const gchar *page = NULL;
    gchar **clean = g_new0(gchar *, argc + 1);
    int clean_count = 0;
    clean[clean_count++] = argv[0];

    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--hidden") == 0) {
            hidden = TRUE;
        } else if (g_str_has_prefix(argv[i], "--screenshot=")) {
            screenshot = argv[i] + strlen("--screenshot=");
        } else if (g_strcmp0(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot = argv[++i];
        } else if (g_str_has_prefix(argv[i], "--page=")) {
            page = argv[i] + strlen("--page=");
        } else {
            clean[clean_count++] = argv[i];
        }
    }
    clean[clean_count] = NULL;

    Y720App *app = y720_app_new(hidden, screenshot, page);
    int status = g_application_run(G_APPLICATION(app), clean_count, clean);
    g_object_unref(app);
    g_free(clean);
    return status;
}
