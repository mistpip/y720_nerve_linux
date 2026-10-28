/* SPDX-License-Identifier: MIT */
#ifndef Y720_BACKLIGHT_PAGE_H
#define Y720_BACKLIGHT_PAGE_H

#include <gtk/gtk.h>
#include "y720-models.h"

G_BEGIN_DECLS

#define Y720_TYPE_BACKLIGHT_PAGE (y720_backlight_page_get_type())
G_DECLARE_FINAL_TYPE(Y720BacklightPage, y720_backlight_page, Y720, BACKLIGHT_PAGE, GtkBox)

GtkWidget *y720_backlight_page_new(void);
void y720_backlight_page_start(Y720BacklightPage *page);
void y720_backlight_page_refresh(Y720BacklightPage *page);
void y720_backlight_page_select_profile(Y720BacklightPage *page, guint index);
const gchar *y720_backlight_page_profile_name(Y720BacklightPage *page, guint index);
guint y720_backlight_page_profile_selected(Y720BacklightPage *page);
void y720_backlight_page_cycle_profile(Y720BacklightPage *page);
void y720_backlight_page_apply_current(Y720BacklightPage *page);
guint y720_backlight_page_profile_count(Y720BacklightPage *page);

G_END_DECLS

#endif
