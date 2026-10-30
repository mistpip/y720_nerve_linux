/* SPDX-License-Identifier: MIT */
/* Exercise the real recording dialog with an injected input reader, never /dev. */
#include "../src/y720-settings-dialog.c"
#include "assert-english.h"

static Y720EvdevCaptureFunc input_callback;
static gpointer input_data;
static guint paused, restored;
static gboolean permitted = TRUE;

void y720_hotkeys_stop(void) { paused++; }
const gchar *y720_hotkeys_status(void) { return "Wayland portal fixture"; }
gboolean y720_evdev_active(void) { return permitted; }
void y720_evdev_capture_stop(void) { input_callback = NULL; input_data = NULL; }
gboolean y720_evdev_capture_start(Y720EvdevCaptureFunc callback, gpointer data, GError **error)
{
    if (!permitted) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "fixture denied");
        return FALSE;
    }
    input_callback = callback;
    input_data = data;
    return TRUE;
}
static void reload(gpointer unused) { (void)unused; restored++; }
static void drain(void)
{
    for (guint i = 0; i < 100; i++) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
}
static GtkWindow *capture_window(void)
{
    GListModel *windows = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
        GtkWindow *window = g_list_model_get_item(windows, i);
        if (g_object_get_data(G_OBJECT(window), "capture"))
            return window;
        g_object_unref(window);
    }
    g_assert_not_reached();
}

int main(void)
{
    g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
    adw_init();
    y720_i18n_set_language("en");
    Y720Settings *settings = y720_settings_new();
    GtkWindow *parent = GTK_WINDOW(adw_window_new());
    gtk_window_present(parent);
    AdwDialog *prefs = y720_settings_dialog_show(parent, settings, TRUE, FALSE, FALSE,
                                                NULL, NULL, reload, NULL, NULL, NULL);
    Dialog *dialog = g_object_get_data(G_OBJECT(prefs), "y720-dialog");
    g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(dialog->global_row)));
    g_assert_true(adw_switch_row_get_active(dialog->global_row));
    assert_english_widget(GTK_WIDGET(prefs));
    on_capture_clicked(GTK_BUTTON(dialog->profile_button), dialog);
    GtkWindow *window = capture_window();
    assert_english_widget(GTK_WIDGET(window));
    g_assert_nonnull(input_callback);
    input_callback(KEY_LEFTCTRL, input_data);
    input_callback(KEY_LEFTSHIFT, input_data);
    input_callback(KEY_KBDILLUMTOGGLE, input_data);
    g_assert_cmpstr(settings->profile_hotkey, ==, "evdev:228");
    g_object_unref(window);
    drain();
    g_assert_cmpuint(restored, ==, 1);
    g_assert_null(input_callback);

    /* Without input permission, ordinary X11/GTK combinations remain recordable. */
    permitted = FALSE;
    on_capture_clicked(GTK_BUTTON(dialog->boost_button), dialog);
    window = capture_window();
    assert_english_widget(GTK_WIDGET(window));
    Capture *capture = g_object_get_data(G_OBJECT(window), "capture");
    GtkEventController *controller = gtk_event_controller_key_new();
    gtk_widget_add_controller(GTK_WIDGET(window), controller);
    capture_key(GTK_EVENT_CONTROLLER_KEY(controller), GDK_KEY_parenright, 19,
                GDK_CONTROL_MASK | GDK_SHIFT_MASK, capture);
    g_object_unref(window);
    drain();
    g_assert_cmpuint(restored, ==, 2);
    guint keyval;
    GdkModifierType modifiers;
    g_assert_true(gtk_accelerator_parse(settings->boost_hotkey, &keyval, &modifiers));
    g_assert_cmpuint(keyval, ==, GDK_KEY_0);
    g_assert_cmpuint(modifiers, ==, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

    permitted = TRUE;
    on_capture_clicked(GTK_BUTTON(dialog->profile_button), dialog);
    window = capture_window();
    gtk_window_destroy(window);  /* Cancel without replacing the binding. */
    g_object_unref(window);
    drain();
    g_assert_cmpuint(restored, ==, 3);
    g_assert_cmpstr(settings->profile_hotkey, ==, "evdev:228");

    on_capture_clicked(GTK_BUTTON(dialog->profile_button), dialog);
    gtk_window_destroy(parent);  /* Parent shutdown also releases the recording dialog. */
    drain();
    g_assert_cmpuint(restored, ==, 4);
    g_assert_cmpuint(paused, ==, 4);
    g_assert_null(input_callback);
    y720_settings_free(settings);
    return 0;
}
