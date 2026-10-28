/* SPDX-License-Identifier: MIT */
#ifndef Y720_FAN_PAGE_H
#define Y720_FAN_PAGE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define Y720_TYPE_FAN_PAGE (y720_fan_page_get_type())
G_DECLARE_FINAL_TYPE(Y720FanPage, y720_fan_page, Y720, FAN_PAGE, GtkBox)

GtkWidget *y720_fan_page_new(void);
void y720_fan_page_start(Y720FanPage *page);
void y720_fan_page_stop(Y720FanPage *page);
void y720_fan_page_refresh(Y720FanPage *page);
void y720_fan_page_set_mode(Y720FanPage *page, const gchar *mode);
void y720_fan_page_toggle_boost(Y720FanPage *page);
void y720_fan_page_set_curve_enabled(Y720FanPage *page, gboolean enabled, const gint *points);
void y720_fan_page_get_curve_points(Y720FanPage *page, gint *points);
void y720_fan_page_curve_start(Y720FanPage *page);
void y720_fan_page_curve_stop(Y720FanPage *page);
void y720_fan_page_curve_toggle(Y720FanPage *page);
gboolean y720_fan_page_curve_active(Y720FanPage *page);
gboolean y720_fan_page_controls_available(Y720FanPage *page);
const gchar *y720_fan_page_mode(Y720FanPage *page);
gint y720_fan_page_rpm(Y720FanPage *page);

G_END_DECLS

#endif
