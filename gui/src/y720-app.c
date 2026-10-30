/* SPDX-License-Identifier: MIT */
#include "y720-app.h"
#include "y720-i18n.h"

#include <gdk/gdk.h>
#if defined(HAVE_X11) && defined(GDK_WINDOWING_X11)
#include <gdk/x11/gdkx.h>
#endif
#include <graphene.h>

#include "y720-hotkeys.h"
#include "y720-portal-hotkeys.h"
#include "y720-evdev.h"
#include "y720-settings.h"
#include "y720-tray.h"
#include "y720-window.h"
#include "y720-cli.h"

struct _Y720App {
    AdwApplication parent_instance;

    Y720Settings *settings;
    Y720Window *window;
    Y720Tray *tray;
    gboolean hidden;
    gboolean quitting;
    gboolean tray_available;
    gboolean hotkey_ok;
    gboolean held;
    gboolean x11_session;
    guint quit_wait;
    gint64 quit_deadline;
    gchar *screenshot_path;
    gchar *screenshot_page;
    gint screenshot_attempts;
};

G_DEFINE_TYPE(Y720App, y720_app, ADW_TYPE_APPLICATION)

static void set_accels(Y720App *self);

static void on_hotkey_state(GObject *owner)
{
    Y720App *self = Y720_APP(owner);
    if (!self->quitting)
        set_accels(self);
}

static void update_tray(Y720App *self)
{
    if (self->quitting || !self->tray || !self->window)
        return;
    guint count = y720_window_profile_count(self->window);
    const gchar **names = g_new0(const gchar *, count + 1);
    for (guint i = 0; i < count; i++)
        names[i] = y720_window_profile_name(self->window, i);
    y720_tray_set_profiles(self->tray, names, count,
                           y720_window_profile_selected(self->window));
    g_free(names);
    y720_tray_set_curve_enabled(self->tray, self->settings->curve_enabled);
    y720_tray_set_state(self->tray,
                         gtk_widget_get_visible(GTK_WIDGET(self->window)),
                         y720_window_fan_controls(self->window),
                         y720_window_fan_mode(self->window),
                         y720_window_fan_rpm(self->window));
}

static void on_tray_action(const gchar *action, gpointer user_data)
{
    Y720App *self = Y720_APP(user_data);
    if (self->quitting)
        return;
    if (g_strcmp0(action, "toggle-window") == 0) {
        gboolean visible = gtk_widget_get_visible(GTK_WIDGET(self->window));
        gtk_widget_set_visible(GTK_WIDGET(self->window), !visible);
        if (!visible)
            gtk_window_present(GTK_WINDOW(self->window));
    } else if (g_strcmp0(action, "mode-auto") == 0) {
        y720_window_set_fan_mode(self->window, "auto");
    } else if (g_strcmp0(action, "mode-boost") == 0) {
        y720_window_set_fan_mode(self->window, "boost");
    } else if (g_strcmp0(action, "mode-curve") == 0) {
        if (self->settings->curve_enabled)
            y720_window_curve_toggle(self->window);
    } else if (g_strcmp0(action, "light-next") == 0) {
        y720_window_cycle_profile(self->window);
    } else if (g_str_has_prefix(action, "profile:")) {
        y720_window_select_profile(self->window, (guint)g_ascii_strtoull(action + 8, NULL, 10));
    } else if (g_strcmp0(action, "settings") == 0) {
        gtk_window_present(GTK_WINDOW(self->window));
        y720_window_show_settings(self->window);
    } else if (g_strcmp0(action, "quit") == 0) {
        g_action_group_activate_action(G_ACTION_GROUP(self), "quit", NULL);
    }
    update_tray(self);
}

static void on_profile_hotkey(gpointer user_data)
{
    Y720App *self = Y720_APP(user_data);
    g_action_group_activate_action(G_ACTION_GROUP(self), "light-next", NULL);
}

static void on_boost_hotkey(gpointer user_data)
{
    Y720App *self = Y720_APP(user_data);
    g_action_group_activate_action(G_ACTION_GROUP(self), "fan-toggle", NULL);
}

static void on_launcher_hotkey(gpointer user_data)
{
    Y720App *self = Y720_APP(user_data);
    if (self->window && !self->quitting) {
        g_debug("Y720 launcher: presenting existing window");
        gtk_window_present(GTK_WINDOW(self->window));
    }
}

static gboolean start_hotkeys(Y720App *self, GError **error)
{
    Y720HotkeyBinding x11[2];
    Y720PortalBinding portal[2];
    Y720HotkeyBinding evdev[3];
    guint x11_count = 0, evdev_count = 0;

    /* The firmware's NumLock-off launcher, never the numeric KEY_KP7. */
    evdev[evdev_count++] = (Y720HotkeyBinding){"evdev:148", on_launcher_hotkey, self};

    const gchar *accels[] = {self->settings->profile_hotkey, self->settings->boost_hotkey};
    Y720HotkeyFunc callbacks[] = {on_profile_hotkey, on_boost_hotkey};
    const gchar *ids[] = {"light-next", "fan-toggle"};
    const gchar *descriptions[] = {y720_tr("切换灯光方案"), y720_tr("切换 auto/boost")};
    for (guint i = 0; i < 2; i++) {
        if (!accels[i] || !*accels[i])
            continue;
        Y720HotkeyBinding b = {accels[i], callbacks[i], self};
        if (g_str_has_prefix(accels[i], "evdev:"))
            evdev[evdev_count++] = b;
        else {
            portal[x11_count] = (Y720PortalBinding){ids[i], descriptions[i], accels[i], callbacks[i]};
            x11[x11_count++] = b;
        }
    }

    gboolean ok = TRUE;
    y720_hotkeys_stop();
    y720_evdev_stop();
    if (x11_count > 0 && self->x11_session && !y720_hotkeys_start(x11, x11_count, error))
        ok = FALSE;
    if (x11_count > 0 && !self->x11_session &&
        !y720_hotkeys_start_portal(portal, x11_count, G_OBJECT(self), on_hotkey_state, error))
        ok = FALSE;
    GError *evdev_error = NULL;
    if (evdev_count > 0 && !y720_evdev_start(evdev, evdev_count, &evdev_error)) {
        ok = FALSE;
        if (error && !*error)
            g_propagate_error(error, evdev_error);
        else
            g_clear_error(&evdev_error);
    }
    return ok;
}

static void on_fan_status_changed(Y720Window *window, gpointer user_data)
{
    (void)window;
    update_tray(Y720_APP(user_data));
}

static void on_profiles_changed(Y720Window *window, gpointer user_data)
{
    (void)window;
    update_tray(Y720_APP(user_data));
}

static void on_visibility_changed(Y720Window *window, gpointer user_data)
{
    (void)window;
    update_tray(Y720_APP(user_data));
}

static gboolean finish_quit(gpointer user_data)
{
    Y720App *self = Y720_APP(user_data);
    if (y720_cli_has_pending() && g_get_monotonic_time() < self->quit_deadline)
        return G_SOURCE_CONTINUE;
    if (y720_cli_has_pending())
        g_warning("CLI cancellation did not finish before the shutdown deadline");
    self->quit_wait = 0;
    if (self->held) {
        g_application_release(G_APPLICATION(self));
        self->held = FALSE;
    }
    g_application_quit(G_APPLICATION(self));
    return G_SOURCE_REMOVE;
}

static void action_quit(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    Y720App *self = Y720_APP(user_data);
    if (self->quitting)
        return;
    self->quitting = TRUE;
    gchar **actions = g_action_group_list_actions(G_ACTION_GROUP(self));
    for (guint i = 0; actions[i]; i++) {
        GAction *entry = g_action_map_lookup_action(G_ACTION_MAP(self), actions[i]);
        if (G_IS_SIMPLE_ACTION(entry))
            g_simple_action_set_enabled(G_SIMPLE_ACTION(entry), FALSE);
    }
    g_strfreev(actions);
    if (!self->held) {
        g_application_hold(G_APPLICATION(self));
        self->held = TRUE;
    }
    if (self->window) {
        y720_window_set_quitting(self->window);
        gtk_window_destroy(GTK_WINDOW(self->window));
    }
    y720_hotkeys_stop();
    y720_evdev_capture_stop();
    y720_evdev_stop();
    y720_cli_cancel_all();
    /* Free the tray in shutdown, not inside its own D-Bus event callback. */
    self->quit_deadline = g_get_monotonic_time() + 3000000;
    self->quit_wait = g_timeout_add(25, finish_quit, self);
}

static void action_show(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    Y720App *self = Y720_APP(user_data);
    gtk_window_present(GTK_WINDOW(self->window));
}

static void action_hide(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    Y720App *self = Y720_APP(user_data);
    gtk_widget_set_visible(GTK_WIDGET(self->window), FALSE);
}

static void action_toggle(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    Y720App *self = Y720_APP(user_data);
    gboolean visible = gtk_widget_get_visible(GTK_WIDGET(self->window));
    if (visible)
        gtk_widget_set_visible(GTK_WIDGET(self->window), FALSE);
    else
        gtk_window_present(GTK_WINDOW(self->window));
}

static void action_settings(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    Y720App *self = Y720_APP(user_data);
    gtk_window_present(GTK_WINDOW(self->window));
    y720_window_show_settings(self->window);
}

static void action_about(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    Y720App *self = Y720_APP(user_data);
    gtk_window_present(GTK_WINDOW(self->window));
    y720_window_show_about(self->window);
}

static void action_fan_auto(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    y720_window_set_fan_mode(Y720_APP(user_data)->window, "auto");
}

static void action_fan_boost(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    y720_window_set_fan_mode(Y720_APP(user_data)->window, "boost");
}

static void action_fan_toggle(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    y720_window_toggle_boost(Y720_APP(user_data)->window);
}

static void action_refresh(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    y720_window_refresh(Y720_APP(user_data)->window);
}

static void action_page_fan(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    y720_window_show_page(Y720_APP(user_data)->window, 0);
}

static void action_light_next(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    Y720App *self = Y720_APP(user_data);
    y720_window_cycle_profile(self->window);
}

static void action_page_backlight(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    y720_window_show_page(Y720_APP(user_data)->window, 1);
}

static void action_autostart(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action;
    (void)parameter;
    Y720App *self = Y720_APP(user_data);
    gboolean active = y720_autostart_enabled();
    GError *error = NULL;
    if (!y720_autostart_set(!active, &error)) {
        y720_window_show_toast(self->window, error->message);
        g_error_free(error);
    } else {
        y720_window_show_toast(self->window, y720_tr(active ? "已关闭开机启动" : "已设置开机启动"));
    }
}

static const GActionEntry app_entries[] = {
    {.name = "quit", .activate = action_quit},
    {.name = "show", .activate = action_show},
    {.name = "hide", .activate = action_hide},
    {.name = "toggle-window", .activate = action_toggle},
    {.name = "settings", .activate = action_settings},
    {.name = "about", .activate = action_about},
    {.name = "fan-auto", .activate = action_fan_auto},
    {.name = "fan-boost", .activate = action_fan_boost},
    {.name = "fan-toggle", .activate = action_fan_toggle},
    {.name = "light-next", .activate = action_light_next},
    {.name = "refresh", .activate = action_refresh},
    {.name = "page-fan", .activate = action_page_fan},
    {.name = "page-backlight", .activate = action_page_backlight},
    {.name = "autostart", .activate = action_autostart},
};

static void set_accels(Y720App *self)
{
    /* A compositor may still deliver an event locally. Never execute a portal
     * shortcut twice, or use the preferred binding after the desktop remaps it.
     * With globals enabled on Wayland, ordinary combos belong to the portal. */
    const gchar *empty[] = {NULL};
    gtk_application_set_accels_for_action(GTK_APPLICATION(self), "app.light-next", empty);
    gtk_application_set_accels_for_action(GTK_APPLICATION(self), "app.fan-toggle", empty);
    gboolean local_combos = self->x11_session || !self->settings->global_hotkey;
    struct { const gchar *action; const gchar *accel; } table[] = {
        {"app.quit", "<Control>q"},
        {"app.settings", "<Control>comma"},
        {"app.page-fan", "<Control>1"},
        {"app.page-backlight", "<Control>2"},
        {"app.refresh", "<Control>r"},
        {"app.fan-auto", "<Control>a"},
        {"app.hide", "<Control>w"},
        {"app.about", "F1"},
    };
    if (local_combos && self->settings->profile_hotkey &&
        !g_str_has_prefix(self->settings->profile_hotkey, "keycode:") &&
        !g_str_has_prefix(self->settings->profile_hotkey, "evdev:")) {
        const gchar *profile_accels[] = {self->settings->profile_hotkey, NULL};
        gtk_application_set_accels_for_action(GTK_APPLICATION(self),
                                              "app.light-next", profile_accels);
    }
    if (local_combos && self->settings->boost_hotkey &&
        !g_str_has_prefix(self->settings->boost_hotkey, "keycode:") &&
        !g_str_has_prefix(self->settings->boost_hotkey, "evdev:")) {
        const gchar *boost_accels[] = {self->settings->boost_hotkey, NULL};
        gtk_application_set_accels_for_action(GTK_APPLICATION(self),
                                              "app.fan-toggle", boost_accels);
    }
    for (guint i = 0; i < G_N_ELEMENTS(table); i++) {
        const gchar *accels[] = {table[i].accel, NULL};
        gtk_application_set_accels_for_action(GTK_APPLICATION(self), table[i].action, accels);
    }
}

static void y720_app_startup(GApplication *application)
{
    G_APPLICATION_CLASS(y720_app_parent_class)->startup(application);
    Y720App *self = Y720_APP(application);

    g_application_set_application_id(application, "org.y720.Control");
    g_application_set_resource_base_path(application, "/org/y720/Control");

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(provider, "/org/y720/Control/style.css");
    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        GtkIconTheme *icons = gtk_icon_theme_get_for_display(display);
        gtk_icon_theme_add_resource_path(icons, "/org/y720/Control/icons");
    }
    g_object_unref(provider);

    self->settings = y720_settings_new();
    y720_i18n_set_language(self->settings->language);
#if defined(HAVE_X11) && defined(GDK_WINDOWING_X11)
    self->x11_session = display && GDK_IS_X11_DISPLAY(display) &&
                         g_strcmp0(g_getenv("XDG_SESSION_TYPE"), "wayland") != 0;
#endif
    AdwStyleManager *style = adw_style_manager_get_default();
    adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_DEFAULT);

    g_action_map_add_action_entries(G_ACTION_MAP(application), app_entries,
                                    G_N_ELEMENTS(app_entries), self);
    set_accels(self);

    if (self->settings->global_hotkey) {
        GError *error = NULL;
        self->hotkey_ok = start_hotkeys(self, &error);
        if (!self->hotkey_ok) {
            g_message("global hotkey unavailable: %s", error->message);
            g_error_free(error);
        }
    }
}

static gboolean show_backlight_later(gpointer data)
{
    y720_window_show_page(Y720_WINDOW(data), 1);
    return G_SOURCE_REMOVE;
}

static gboolean show_settings_later(gpointer data)
{
    y720_window_show_settings(Y720_WINDOW(data));
    return G_SOURCE_REMOVE;
}

static gboolean render_window(gpointer user_data)
{
    Y720App *self = Y720_APP(user_data);
    GtkWidget *widget = y720_window_dialog(self->window);
    if (!widget)
        widget = GTK_WIDGET(self->window);
    gint width = gtk_widget_get_width(widget);
    gint height = gtk_widget_get_height(widget);
    if (width <= 0 || height <= 0) {
        width = 760;
        height = 620;
    }

    GdkPaintable *paintable = gtk_widget_paintable_new(widget);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snapshot, width, height);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    GtkNative *native = gtk_widget_get_native(widget);
    GskRenderer *renderer = native ? gtk_native_get_renderer(native) : NULL;

    if ((!renderer || !node) && self->screenshot_attempts < 6) {
        self->screenshot_attempts++;
        if (node)
            gsk_render_node_unref(node);
        g_object_unref(paintable);
        g_timeout_add(700, render_window, self);
        return G_SOURCE_REMOVE;
    }

    if (renderer && node) {
        graphene_rect_t rect = GRAPHENE_RECT_INIT(0, 0, width, height);
        GdkTexture *texture = gsk_renderer_render_texture(renderer, node, &rect);
        if (texture) {
            gdk_texture_save_to_png(texture, self->screenshot_path);
            g_object_unref(texture);
        }
    }
    if (node)
        gsk_render_node_unref(node);
    g_object_unref(paintable);

    g_application_quit(G_APPLICATION(self));
    return G_SOURCE_REMOVE;
}

static void y720_app_activate(GApplication *application)
{
    Y720App *self = Y720_APP(application);
    if (self->quitting)
        return;
    if (self->window) {
        self->hidden = FALSE;
        gtk_window_present(GTK_WINDOW(self->window));
        return;
    }

    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (bus) {
        self->tray = y720_tray_new(bus, on_tray_action, self);
        self->tray_available = y720_tray_is_available(self->tray);
        if (!self->tray_available) {
            y720_tray_free(self->tray);
            self->tray = NULL;
        } else {
            /* Closing a transient settings window must not end a tray app. */
            g_application_hold(application);
            self->held = TRUE;
        }
        g_object_unref(bus);
    } else {
        g_message("session bus unavailable: %s", error->message);
        g_error_free(error);
    }

    self->window = y720_window_new(ADW_APPLICATION(application), self->settings,
                                   self->tray_available, self->hotkey_ok, self->x11_session);
    g_object_add_weak_pointer(G_OBJECT(self->window), (gpointer *)&self->window);
    g_signal_connect(self->window, "fan-status", G_CALLBACK(on_fan_status_changed), self);
    g_signal_connect(self->window, "visibility-changed", G_CALLBACK(on_visibility_changed), self);
    g_signal_connect(self->window, "profiles-changed", G_CALLBACK(on_profiles_changed), self);
    y720_window_start(self->window);

    if (self->screenshot_path) {
        self->hidden = FALSE;
        gtk_window_present(GTK_WINDOW(self->window));
        if (self->screenshot_page && g_strcmp0(self->screenshot_page, "backlight") == 0) {
            g_timeout_add(700, show_backlight_later, self->window);
        } else if (self->screenshot_page && g_strcmp0(self->screenshot_page, "settings") == 0) {
            g_timeout_add(700, show_settings_later, self->window);
        }
        g_timeout_add(1800, render_window, self);
    } else if (!self->hidden) {
        gtk_window_present(GTK_WINDOW(self->window));
    } else if (!self->tray_available) {
        g_warning("tray unavailable; showing window instead of staying hidden");
        gtk_window_present(GTK_WINDOW(self->window));
    }

    update_tray(self);
}

void y720_app_reload_shortcut(Y720App *self)
{
    if (self->quitting)
        return;
    y720_hotkeys_stop();
    y720_evdev_stop();
    self->hotkey_ok = FALSE;
    if (self->settings->global_hotkey) {
        GError *error = NULL;
        self->hotkey_ok = start_hotkeys(self, &error);
        if (!self->hotkey_ok) {
            g_message("global hotkey unavailable: %s", error->message);
            g_error_free(error);
        }
    }
    const gchar *empty[] = {NULL};
    gtk_application_set_accels_for_action(GTK_APPLICATION(self), "app.light-next", empty);
    gtk_application_set_accels_for_action(GTK_APPLICATION(self), "app.fan-toggle", empty);
    set_accels(self);
    update_tray(self);
}

static void y720_app_shutdown(GApplication *application)
{
    Y720App *self = Y720_APP(application);
    self->quitting = TRUE;
    if (self->quit_wait) {
        g_source_remove(self->quit_wait);
        self->quit_wait = 0;
    }
    y720_hotkeys_stop();
    y720_evdev_capture_stop();
    y720_evdev_stop();
    if (self->window) {
        y720_window_set_quitting(self->window);
        gtk_window_destroy(GTK_WINDOW(self->window));
    }
    if (self->held) {
        g_application_release(G_APPLICATION(self));
        self->held = FALSE;
    }
    if (self->tray) {
        y720_tray_free(self->tray);
        self->tray = NULL;
    }
    G_APPLICATION_CLASS(y720_app_parent_class)->shutdown(application);
}

static void y720_app_dispose(GObject *object)
{
    Y720App *self = Y720_APP(object);
    g_clear_pointer(&self->settings, y720_settings_free);
    g_clear_pointer(&self->screenshot_path, g_free);
    g_clear_pointer(&self->screenshot_page, g_free);
    G_OBJECT_CLASS(y720_app_parent_class)->dispose(object);
}

static void y720_app_class_init(Y720AppClass *klass)
{
    GApplicationClass *application_class = G_APPLICATION_CLASS(klass);
    application_class->startup = y720_app_startup;
    application_class->activate = y720_app_activate;
    application_class->shutdown = y720_app_shutdown;
    G_OBJECT_CLASS(klass)->dispose = y720_app_dispose;
}

static void y720_app_init(Y720App *self)
{
    self->x11_session = FALSE;
}

Y720App *y720_app_new(gboolean hidden, const gchar *screenshot_path,
                      const gchar *screenshot_page)
{
    Y720App *self = g_object_new(Y720_TYPE_APP,
                                 "application-id", "org.y720.Control",
                                 "flags", G_APPLICATION_DEFAULT_FLAGS,
                                 NULL);
    self->hidden = hidden;
    self->screenshot_path = g_strdup(screenshot_path);
    self->screenshot_page = g_strdup(screenshot_page);
    return self;
}
