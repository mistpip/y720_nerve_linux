/* SPDX-License-Identifier: MIT */
#include <adwaita.h>
#include "y720-backlight-page.h"
#include "../src/y720-fan-page.c"
#include "y720-i18n.h"
#include <unistd.h>
#include <glib/gstdio.h>
#include "assert-english.h"

static guint applied;
static void profile_applied(Y720BacklightPage *page, gpointer unused)
{
    (void)page; (void)unused;
    applied++;
}

static void drain(guint milliseconds)
{
    gint64 end = g_get_monotonic_time() + milliseconds * 1000;
    while (g_get_monotonic_time() < end) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
}

static GtkWidget *find_label(GtkWidget *widget, const gchar *text)
{
    if (GTK_IS_LABEL(widget) && !g_strcmp0(gtk_label_get_text(GTK_LABEL(widget)), text))
        return widget;
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_label(child, text);
        if (found) return found;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, ==, 2);
    gchar *directory = g_dir_make_tmp("y720-widget-fixture-XXXXXX", NULL);
    g_assert_nonnull(directory);
    gchar *light_binary = g_build_filename(directory, "y720-backlight", NULL);
    g_assert_cmpint(symlink(argv[1], light_binary), ==, 0);
    g_setenv("Y720_FAN_BIN", argv[1], TRUE);
    g_setenv("Y720_BACKLIGHT_BIN", light_binary, TRUE);
    g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
    adw_init();
    for (guint i = 0; i < 4; i++) {
        GtkWidget *window = gtk_window_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget *light = y720_backlight_page_new();
        GtkWidget *fan = y720_fan_page_new();
        gtk_box_append(GTK_BOX(box), light);
        gtk_box_append(GTK_BOX(box), fan);
        gtk_window_set_child(GTK_WINDOW(window), box);
        gtk_window_present(GTK_WINDOW(window));
        y720_backlight_page_start(Y720_BACKLIGHT_PAGE(light));
        y720_fan_page_start(Y720_FAN_PAGE(fan));
        drain(i % 2 ? 150 : 0); /* Destroy both before and after async completion. */
        if (i % 2) {
            g_assert_true(y720_fan_page_controls_available(Y720_FAN_PAGE(fan)));
            y720_fan_page_set_mode(Y720_FAN_PAGE(fan), "auto");
            y720_backlight_page_apply_current(Y720_BACKLIGHT_PAGE(light));
        }
        gtk_window_destroy(GTK_WINDOW(window));
        drain(250);
    }
    /* One kernel temperature must refresh and clear stale values in English. */
    y720_i18n_set_language("en");
    GtkWidget *temperature_window = gtk_window_new();
    GtkWidget *fan = y720_fan_page_new();
    gtk_window_set_child(GTK_WINDOW(temperature_window), fan);
    y720_fan_page_start(Y720_FAN_PAGE(fan));
    drain(300);
    GtkWidget *reading = find_label(fan, "55 °C");
    g_assert_nonnull(reading);
    g_assert_cmpstr(gtk_widget_get_tooltip_text(reading), ==, "Kernel control temperature from the CPU package sensor");
    g_setenv("Y720_FIXTURE_CPU_MC", "61000", TRUE);
    y720_fan_page_refresh(Y720_FAN_PAGE(fan));
    drain(300);
    g_assert_nonnull(find_label(fan, "61 °C"));
    g_assert_null(find_label(fan, "55 °C"));
    g_setenv("Y720_FIXTURE_QUERY_FAIL", "1", TRUE);
    y720_fan_page_refresh(Y720_FAN_PAGE(fan));
    drain(300);
    g_assert_null(find_label(fan, "61 °C"));
    g_assert_false(y720_fan_page_controls_available(Y720_FAN_PAGE(fan)));
    g_unsetenv("Y720_FIXTURE_QUERY_FAIL");
    y720_fan_page_refresh(Y720_FAN_PAGE(fan));
    drain(300);
    g_assert_nonnull(find_label(fan, "61 °C"));
    gtk_window_destroy(GTK_WINDOW(temperature_window));
    drain(250);
    g_unsetenv("Y720_FIXTURE_CPU_MC");
    gchar *curve_state = g_build_filename(directory, "curve-state", NULL);
    g_setenv("Y720_FIXTURE_CURVE_STATE", curve_state, TRUE);
    GtkWidget *curve_window = gtk_window_new();
    fan = y720_fan_page_new();
    gtk_window_set_child(GTK_WINDOW(curve_window), fan);
    const gint points[] = {1500,1800,2400,3200,4200,5100};
    y720_fan_page_set_curve_enabled(Y720_FAN_PAGE(fan), TRUE, points);
    y720_fan_page_start(Y720_FAN_PAGE(fan));
    drain(300);
    g_setenv("Y720_FIXTURE_CURVE_FAIL", "1", TRUE);
    y720_fan_page_curve_start(Y720_FAN_PAGE(fan));
    drain(350);
    g_assert_false(y720_fan_page_curve_active(Y720_FAN_PAGE(fan)));
    g_assert_false(g_file_test(curve_state, G_FILE_TEST_EXISTS));
    g_unsetenv("Y720_FIXTURE_CURVE_FAIL");
    y720_fan_page_curve_start(Y720_FAN_PAGE(fan));
    drain(350);
    g_assert_true(y720_fan_page_curve_active(Y720_FAN_PAGE(fan)));
    y720_i18n_apply(fan);
    assert_english_widget(fan);
    Y720FanPage *page = Y720_FAN_PAGE(fan);
    gint edited[] = {1500,1800,2400,3200,4300,5100};
    y720_fan_curve_set_points(Y720_FAN_CURVE(page->curve_editor), edited);
    on_curve_point_changed(page);
    drain(100);
    edited[4] = 4400;
    y720_fan_curve_set_points(Y720_FAN_CURVE(page->curve_editor), edited);
    on_curve_point_changed(page);
    drain(600);
    gchar *edited_state = NULL;
    g_assert_true(g_file_get_contents(curve_state, &edited_state, NULL, NULL));
    g_assert_nonnull(strstr(edited_state, "4400"));
    g_free(edited_state);
    /* A late acknowledgement must not replace an edit made while it ran. */
    g_setenv("Y720_FIXTURE_CURVE_DELAY", "1", TRUE);
    curve_set_active(page, TRUE);
    edited[4] = 4500;
    y720_fan_curve_set_points(Y720_FAN_CURVE(page->curve_editor), edited);
    on_curve_point_changed(page);
    drain(800);
    g_unsetenv("Y720_FIXTURE_CURVE_DELAY");
    g_assert_true(g_file_get_contents(curve_state, &edited_state, NULL, NULL));
    g_assert_nonnull(strstr(edited_state, "4500"));
    g_free(edited_state);
    g_setenv("Y720_FIXTURE_CURVE_ERROR", "1", TRUE);
    y720_fan_page_refresh(Y720_FAN_PAGE(fan));
    drain(300);
    g_assert_true(y720_fan_page_curve_active(Y720_FAN_PAGE(fan)));
    g_unsetenv("Y720_FIXTURE_CURVE_ERROR");
    gtk_window_destroy(GTK_WINDOW(curve_window));
    drain(300);
    gchar *saved = NULL;
    g_assert_true(g_file_get_contents(curve_state, &saved, NULL, NULL));
    g_assert_true(g_str_has_prefix(saved, "3 ")); /* Exit does not stop the kernel. */
    g_free(saved);
    /* Another hwmon controller takes ownership: refresh must not retake it. */
    g_assert_true(g_file_set_contents(curve_state, "1 1500 1800 2400 3200 4200 5100\n", -1, NULL));
    curve_window = gtk_window_new();
    fan = y720_fan_page_new();
    gtk_window_set_child(GTK_WINDOW(curve_window), fan);
    y720_fan_page_set_curve_enabled(Y720_FAN_PAGE(fan), TRUE, points);
    y720_fan_page_start(Y720_FAN_PAGE(fan));
    drain(300);
    g_assert_false(y720_fan_page_curve_active(Y720_FAN_PAGE(fan)));
    gtk_window_destroy(GTK_WINDOW(curve_window));
    drain(300);
    g_assert_true(g_file_get_contents(curve_state, &saved, NULL, NULL));
    g_assert_true(g_str_has_prefix(saved, "1 "));
    g_free(saved);
    g_unsetenv("Y720_FIXTURE_CURVE_STATE");
    g_unlink(curve_state);
    g_free(curve_state);
    /* A zero exit status with data={} must not count as a lighting submission. */
    GtkWidget *window = gtk_window_new();
    GtkWidget *light = y720_backlight_page_new();
    gtk_window_set_child(GTK_WINDOW(window), light);
    g_signal_connect(light, "profile-applied", G_CALLBACK(profile_applied), NULL);
    y720_backlight_page_start(Y720_BACKLIGHT_PAGE(light));
    drain(250);
    y720_i18n_apply(light);
    assert_english_widget(light);
    y720_backlight_page_apply_current(Y720_BACKLIGHT_PAGE(light));
    drain(250);
    g_assert_cmpuint(applied, ==, 1);
    g_setenv("Y720_FIXTURE_BAD_ACK", "1", TRUE);
    y720_backlight_page_apply_current(Y720_BACKLIGHT_PAGE(light));
    g_unsetenv("Y720_FIXTURE_BAD_ACK");
    drain(250);
    g_assert_cmpuint(applied, ==, 1);
    gtk_window_destroy(GTK_WINDOW(window));
    drain(1500);
    g_unlink(light_binary);
    g_rmdir(directory);
    g_free(light_binary);
    g_free(directory);
    return 0;
}
