/* SPDX-License-Identifier: MIT */
#ifndef Y720_I18N_H
#define Y720_I18N_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

gboolean y720_i18n_english(void);
const gchar *y720_tr(const gchar *text);
void y720_i18n_apply(GtkWidget *root);
void y720_i18n_set_language(const gchar *language);
/* Call before GTK initialization so toolkit-owned labels use the same language. */
void y720_i18n_prepare_environment(const gchar *language);

G_END_DECLS

#endif
