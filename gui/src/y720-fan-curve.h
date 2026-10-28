/* SPDX-License-Identifier: MIT */
#ifndef Y720_FAN_CURVE_H
#define Y720_FAN_CURVE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define Y720_FAN_CURVE_POINTS 6
#define Y720_TYPE_FAN_CURVE (y720_fan_curve_get_type())
G_DECLARE_FINAL_TYPE(Y720FanCurve, y720_fan_curve, Y720, FAN_CURVE, GtkDrawingArea)

typedef void (*Y720FanCurveFunc)(gpointer user_data);

GtkWidget *y720_fan_curve_new(void);
void y720_fan_curve_set_points(Y720FanCurve *curve, const gint *points);
void y720_fan_curve_get_points(Y720FanCurve *curve, gint *points);
void y720_fan_curve_set_callback(Y720FanCurve *curve, Y720FanCurveFunc callback,
                                 gpointer user_data);

G_END_DECLS

#endif
