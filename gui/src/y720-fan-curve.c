/* SPDX-License-Identifier: MIT */
#include "y720-fan-curve.h"

struct _Y720FanCurve {
    GtkDrawingArea parent_instance;
    gint rpms[Y720_FAN_CURVE_POINTS];
    gint selected;
    Y720FanCurveFunc callback;
    gpointer callback_data;
};

G_DEFINE_TYPE(Y720FanCurve, y720_fan_curve, GTK_TYPE_DRAWING_AREA)

static const gint temperatures[Y720_FAN_CURVE_POINTS] = {40, 50, 60, 70, 80, 90};

static void point_xy(Y720FanCurve *self, gint width, gint height, guint i,
                     gdouble *x, gdouble *y)
{
    (void)self;
    const gdouble left = 58, right = 22, top = 24, bottom = 46;
    gdouble w = width - left - right;
    gdouble h = height - top - bottom;
    *x = left + w * i / (Y720_FAN_CURVE_POINTS - 1);
    gdouble t = (self->rpms[i] - 1500) / 3600.0;
    *y = top + h * (1.0 - CLAMP(t, 0.0, 1.0));
}

static void draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data)
{
    (void)area;
    Y720FanCurve *self = Y720_FAN_CURVE(user_data);
    const gdouble left = 58, right = 22, top = 24, bottom = 46;
    gdouble w = width - left - right;
    gdouble h = height - top - bottom;

    GdkRGBA fg = {0.5, 0.5, 0.5, 1.0};
    gtk_widget_get_color(GTK_WIDGET(self), &fg);

    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.12);
    for (gint rpm = 1500; rpm <= 5100; rpm += 1200) {
        gdouble y = top + h * (1.0 - (rpm - 1500) / 3600.0);
        cairo_move_to(cr, left, y);
        cairo_line_to(cr, left + w, y);
        cairo_stroke(cr);
    }
    for (guint i = 0; i < Y720_FAN_CURVE_POINTS; i++) {
        gdouble x = left + w * i / (Y720_FAN_CURVE_POINTS - 1);
        cairo_move_to(cr, x, top);
        cairo_line_to(cr, x, top + h);
        cairo_stroke(cr);
    }

    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.45);
    cairo_set_line_width(cr, 1.2);
    cairo_move_to(cr, left, top);
    cairo_line_to(cr, left, top + h);
    cairo_line_to(cr, left + w, top + h);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 0.75, 0.12, 0.18, 1.0);
    cairo_set_line_width(cr, 2.4);
    for (guint i = 0; i < Y720_FAN_CURVE_POINTS; i++) {
        gdouble x, y;
        point_xy(self, width, height, i, &x, &y);
        if (i == 0) cairo_move_to(cr, x, y);
        else cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);

    for (guint i = 0; i < Y720_FAN_CURVE_POINTS; i++) {
        gdouble x, y;
        point_xy(self, width, height, i, &x, &y);
        cairo_arc(cr, x, y, i == (guint)self->selected ? 7 : 4, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 1, 1, 1, 1);
        cairo_fill(cr);
    }

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.65);
    for (guint i = 0; i < Y720_FAN_CURVE_POINTS; i++) {
        gchar label[16];
        g_snprintf(label, sizeof(label), "%d", temperatures[i]);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, label, &ext);
        gdouble x = left + w * i / (Y720_FAN_CURVE_POINTS - 1);
        cairo_move_to(cr, x - ext.width / 2.0, top + h + 17);
        cairo_show_text(cr, label);
    }
    for (gint rpm = 1500; rpm <= 5100; rpm += 1200) {
        gchar label[16];
        g_snprintf(label, sizeof(label), "%d", rpm);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, label, &ext);
        gdouble y = top + h * (1.0 - (rpm - 1500) / 3600.0);
        cairo_move_to(cr, left - 8 - ext.width, y + ext.height / 2.0);
        cairo_show_text(cr, label);
    }

    cairo_set_font_size(cr, 10);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.75);
    const gchar *x_title = "Temperature (°C)";
    cairo_text_extents_t ext;
    cairo_text_extents(cr, x_title, &ext);
    cairo_move_to(cr, left + (w - ext.width) / 2.0, height - 10);
    cairo_show_text(cr, x_title);

    cairo_save(cr);
    cairo_translate(cr, 12, top + h / 2.0);
    cairo_rotate(cr, -M_PI / 2.0);
    const gchar *y_title = "Fan speed (RPM)";
    cairo_text_extents(cr, y_title, &ext);
    cairo_move_to(cr, -ext.width / 2.0, 0);
    cairo_show_text(cr, y_title);
    cairo_restore(cr);
}

static void on_drag(GtkGestureDrag *gesture, gdouble offset_x, gdouble offset_y,
                    gpointer user_data)
{
    Y720FanCurve *self = Y720_FAN_CURVE(user_data);
    GtkWidget *area = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    gdouble start_x, start_y;
    gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
    gdouble x = start_x + offset_x;
    gint width = gtk_widget_get_width(area);
    gint height = gtk_widget_get_height(area);
    const gdouble left = 58, right = 22, top = 24, bottom = 46;
    gdouble w = width - left - right;
    gdouble h = height - top - bottom;
    gint nearest = 0;
    gdouble best = 1e9;
    for (guint i = 0; i < Y720_FAN_CURVE_POINTS; i++) {
        gdouble px = left + w * i / (Y720_FAN_CURVE_POINTS - 1);
        gdouble d = ABS(px - x);
        if (d < best) { best = d; nearest = (gint)i; }
    }
    gdouble y = start_y + offset_y;
    gdouble t = 1.0 - (y - top) / h;
    gint rpm = (gint)((1500 + t * 3600.0) / 100.0 + 0.5) * 100;
    gint low = nearest ? self->rpms[nearest - 1] : 1500;
    gint high = nearest + 1 < Y720_FAN_CURVE_POINTS ? self->rpms[nearest + 1] : 5100;
    self->rpms[nearest] = CLAMP(rpm, low, high);
    self->selected = nearest;
    gtk_widget_queue_draw(area);
    if (self->callback)
        self->callback(self->callback_data);
}

static void y720_fan_curve_class_init(Y720FanCurveClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    gtk_widget_class_set_css_name(widget_class, "fan-curve");
}

static void y720_fan_curve_init(Y720FanCurve *self)
{
    const gint defaults[Y720_FAN_CURVE_POINTS] = {1500, 1800, 2400, 3200, 4200, 5100};
    memcpy(self->rpms, defaults, sizeof(defaults));
    self->selected = 0;
    gtk_widget_set_size_request(GTK_WIDGET(self), 560, 190);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(self), draw, self, NULL);
    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(drag));
}

GtkWidget *y720_fan_curve_new(void)
{
    return g_object_new(Y720_TYPE_FAN_CURVE, NULL);
}

void y720_fan_curve_set_points(Y720FanCurve *self, const gint *points)
{
    g_return_if_fail(Y720_IS_FAN_CURVE(self));
    for (guint i = 0; i < Y720_FAN_CURVE_POINTS; i++)
        self->rpms[i] = CLAMP(points[i], 1500, 5100);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void y720_fan_curve_get_points(Y720FanCurve *self, gint *points)
{
    for (guint i = 0; i < Y720_FAN_CURVE_POINTS; i++)
        points[i] = self->rpms[i];
}

void y720_fan_curve_set_callback(Y720FanCurve *self, Y720FanCurveFunc callback,
                                 gpointer user_data)
{
    self->callback = callback;
    self->callback_data = user_data;
}
