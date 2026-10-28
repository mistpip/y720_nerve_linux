/* SPDX-License-Identifier: MIT */
#include "y720-keyboard.h"

#include <math.h>

typedef struct {
    gdouble x, y, w, h;
    gint segment;
} KeyRect;

typedef struct {
    gint colors[4];
    gint brightness[4];
    gint selected;
    Y720KeyboardCallback callback;
    gpointer callback_data;
    GArray *keys;
    gint width;
    gint height;
} KeyboardData;

static const gchar *palette[20] = {
    "#e01b24", "#ff5a36", "#ff6fa5", "#ff2bd6", "#a55eea",
    "#77767b", "#3584e4", "#1c71d8", "#62a0ea", "#33d6e0",
    "#26a269", "#2ec27e", "#33d17a", "#8ff0a4", "#c0bf6b",
    "#f6d32d", "#ff7800", "#e66100", "#f6f5f4", "#000000",
};

static void key_color(gint index, GdkRGBA *rgba)
{
    gdk_rgba_parse(rgba, palette[CLAMP(index, 0, 19)]);
}

static gint segment_for(gint row, gint index, gboolean numpad)
{
    if (numpad)
        return 3;
    if (index < 4)
        return 0;
    if (index < 9)
        return (row == 5 && index >= 5) ? 2 : 1;
    return 2;
}

static void add_key(KeyboardData *data, gdouble x, gdouble y, gdouble w, gdouble h, gint segment)
{
    KeyRect key = {x, y, w, h, segment};
    g_array_append_val(data->keys, key);
}

static void layout_keyboard(KeyboardData *data, gint width, gint height)
{
    g_array_set_size(data->keys, 0);
    data->width = width;
    data->height = height;

    const gdouble main_rows[][16] = {
        {1.2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1.8},
        {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2},
        {2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
        {2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2},
        {2.5, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2.5},
        {1.5, 1, 1, 1, 7, 1, 1, 1.5},
    };
    const guint row_lengths[] = {14, 14, 14, 13, 12, 8};
    const gdouble main_width = 15.0;
    const gdouble numpad_gap = 0.18;
    const gdouble numpad_width = 4.0;
    const gdouble total_width = main_width + numpad_gap + numpad_width;
    const gdouble pad = 6.0;
    const gdouble unit = MIN((width - pad * 2.0) / total_width,
                             (height - pad * 2.0) / 6.0);
    const gdouble gap = unit * 0.09;
    const gdouble start_y = (height - (6.0 * unit + 5.0 * gap)) / 2.0;

    for (guint row = 0; row < 6; row++) {
        gdouble x = pad;
        for (guint i = 0; i < row_lengths[row]; i++) {
            gdouble w = main_rows[row][i] * unit;
            add_key(data, x + gap / 2.0, start_y + row * (unit + gap) + gap / 2.0,
                    w - gap, unit - gap, segment_for((gint)row, (gint)i, FALSE));
            x += w;
        }
    }

    const gdouble numpad_x = pad + main_width * unit + numpad_gap * unit;
    const gint numpad_segments[4][4] = {
        {3, 3, 3, 3}, {3, 3, 3, 3}, {3, 3, 3, 3}, {3, 3, 3, 3},
    };
    for (guint row = 0; row < 4; row++) {
        for (guint col = 0; col < 4; col++) {
            add_key(data, numpad_x + col * unit + gap / 2.0,
                    start_y + row * (unit + gap) + gap / 2.0,
                    unit - gap, unit - gap, numpad_segments[row][col]);
        }
    }

    /* Up arrow above the bottom arrow cluster. */
    add_key(data, numpad_x + unit + gap / 2.0,
            start_y + 4 * (unit + gap) + gap / 2.0,
            unit - gap, unit - gap, 3);
    /* Left / Down / Right. */
    for (guint i = 0; i < 3; i++) {
        add_key(data, numpad_x + i * unit + gap / 2.0,
                start_y + 5 * (unit + gap) + gap / 2.0,
                unit - gap, unit - gap, 3);
    }
}

static void draw_rounded_rect(cairo_t *cr, gdouble x, gdouble y, gdouble w, gdouble h, gdouble radius)
{
    gdouble r = MIN(radius, MIN(w, h) / 2.0);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

static void draw_keyboard(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data)
{
    (void)area;
    KeyboardData *data = user_data;
    layout_keyboard(data, width, height);

    for (guint i = 0; i < data->keys->len; i++) {
        KeyRect *key = &g_array_index(data->keys, KeyRect, i);
        gint segment = key->segment;
        GdkRGBA color;
        key_color(data->colors[segment], &color);
        gint brightness = CLAMP(data->brightness[segment], 0, 3);
        gdouble fill = brightness == 0 ? 0.0 : 0.22 + brightness * 0.18;

        draw_rounded_rect(cr, key->x, key->y, key->w, key->h, key->h * 0.16);
        cairo_set_source_rgba(cr, 0.10, 0.10, 0.12, 0.9);
        cairo_fill(cr);

        if (fill > 0.0) {
            draw_rounded_rect(cr, key->x + 1, key->y + 1, key->w - 2, key->h - 2, key->h * 0.14);
            cairo_set_source_rgba(cr, color.red, color.green, color.blue, fill);
            cairo_fill(cr);
        }

        draw_rounded_rect(cr, key->x, key->y, key->w, key->h, key->h * 0.16);
        if (segment == data->selected) {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
            cairo_set_line_width(cr, 2.0);
        } else {
            cairo_set_source_rgba(cr, color.red, color.green, color.blue, 0.55);
            cairo_set_line_width(cr, 1.0);
        }
        cairo_stroke(cr);
    }
}

static void on_pressed(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data)
{
    (void)n_press;
    (void)user_data;
    GtkWidget *area = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    KeyboardData *data = g_object_get_data(G_OBJECT(area), "keyboard-data");
    for (guint i = 0; i < data->keys->len; i++) {
        KeyRect *key = &g_array_index(data->keys, KeyRect, i);
        if (x >= key->x && x <= key->x + key->w &&
            y >= key->y && y <= key->y + key->h) {
            data->selected = key->segment;
            gtk_widget_queue_draw(area);
            if (data->callback)
                data->callback(key->segment, data->callback_data);
            return;
        }
    }
}

static void keyboard_data_free(gpointer data)
{
    KeyboardData *keyboard = data;
    g_array_free(keyboard->keys, TRUE);
    g_free(keyboard);
}

GtkWidget *y720_keyboard_new(void)
{
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, 620, 168);
    gtk_widget_set_hexpand(area, TRUE);
    gtk_widget_set_valign(area, GTK_ALIGN_CENTER);

    KeyboardData *data = g_new0(KeyboardData, 1);
    data->keys = g_array_new(FALSE, FALSE, sizeof(KeyRect));
    data->selected = 0;
    for (guint i = 0; i < 4; i++) {
        data->colors[i] = i * 6;
        data->brightness[i] = 2;
    }
    g_object_set_data_full(G_OBJECT(area), "keyboard-data", data, keyboard_data_free);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_keyboard, data, NULL);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
    g_signal_connect(click, "pressed", G_CALLBACK(on_pressed), NULL);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(click));

    return area;
}

void y720_keyboard_set_profile(GtkWidget *keyboard, const gint colors[4],
                               const gint brightness[4], gint selected)
{
    KeyboardData *data = g_object_get_data(G_OBJECT(keyboard), "keyboard-data");
    if (!data)
        return;
    memcpy(data->colors, colors, sizeof(data->colors));
    memcpy(data->brightness, brightness, sizeof(data->brightness));
    data->selected = CLAMP(selected, 0, 3);
    gtk_widget_queue_draw(keyboard);
}

void y720_keyboard_set_selected(GtkWidget *keyboard, gint selected)
{
    KeyboardData *data = g_object_get_data(G_OBJECT(keyboard), "keyboard-data");
    if (!data)
        return;
    data->selected = CLAMP(selected, 0, 3);
    gtk_widget_queue_draw(keyboard);
}

gint y720_keyboard_get_selected(GtkWidget *keyboard)
{
    KeyboardData *data = g_object_get_data(G_OBJECT(keyboard), "keyboard-data");
    return data ? data->selected : 0;
}

void y720_keyboard_set_callback(GtkWidget *keyboard, Y720KeyboardCallback callback,
                                gpointer user_data)
{
    KeyboardData *data = g_object_get_data(G_OBJECT(keyboard), "keyboard-data");
    if (!data)
        return;
    data->callback = callback;
    data->callback_data = user_data;
}
