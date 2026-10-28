/* SPDX-License-Identifier: MIT */
#include "y720-app.h"

#include <gdk/gdk.h>
#include <gdk/x11/gdkx.h>
#include <graphene.h>

#include "y720-hotkeys.h"
#include "y720-settings.h"
#include "y720-tray.h"
#include "y720-window.h"

struct _Y720App {
    AdwApplication parent_instance;

    Y720Settings *settings;
    Y720Window *window;
    Y720Tray *tray;
    gboolean hidden;
    gboolean quitting;
    gboolean tray_available;
    gboolean hotkey_ok;
    gboolean x11_session;
    gchar *screenshot_path;
    gchar *screenshot_page;
    gint screenshot_attempts;
};

G_DEFINE_TYPE(Y720App, y720_app, ADW_TYPE_APPLICATION)

static void update_tray(Y720App *self)
{
    if (!self->tray || !self->window)
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
    g_action_group_activate_action(G_ACTION_GROUP(self), "fan-boost", NULL);
}

static gboolean start_hotkeys(Y720App *self, GError **error)
{
    Y720HotkeyBinding bindings[] = {
        {self->settings->profile_hotkey, on_profile_hotkey, self},
        {self->settings->boost_hotkey, on_boost_hotkey, self},
    };
    return y720_hotkeys_start(bindings, G_N_ELEMENTS(bindings), error);
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

static void action_quit(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    (void)action; (void)parameter;
    Y720App *self = Y720_APP(user_data);
    self->quitting = TRUE;
    y720_window_set_quitting(self->window);
    y720_hotkeys_stop();
    if (self->tray) {
        y720_tray_free(self->tray);
        self->tray = NULL;
    }
    g_application_quit(G_APPLICATION(self));
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
        y720_window_show_toast(self->window, active ? "已关闭开机启动" : "已设置开机启动");
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
    if (self->settings->profile_hotkey) {
        const gchar *profile_accels[] = {self->settings->profile_hotkey, NULL};
        gtk_application_set_accels_for_action(GTK_APPLICATION(self),
                                              "app.light-next", profile_accels);
    }
    if (self->settings->boost_hotkey) {
        const gchar *boost_accels[] = {self->settings->boost_hotkey, NULL};
        gtk_application_set_accels_for_action(GTK_APPLICATION(self),
                                              "app.fan-boost", boost_accels);
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
    }
    g_object_unref(provider);

    self->settings = y720_settings_new();
    self->x11_session = display && GDK_IS_X11_DISPLAY(display);
    AdwStyleManager *style = adw_style_manager_get_default();
    adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_FORCE_DARK);

    g_action_map_add_action_entries(G_ACTION_MAP(application), app_entries,
                                    G_N_ELEMENTS(app_entries), self);
    set_accels(self);

    if (self->settings->global_hotkey && self->x11_session) {
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
        }
        g_object_unref(bus);
    } else {
        g_message("session bus unavailable: %s", error->message);
        g_error_free(error);
    }

    self->window = y720_window_new(ADW_APPLICATION(application), self->settings,
                                   self->tray_available, self->hotkey_ok, self->x11_session);
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
    y720_hotkeys_stop();
    self->hotkey_ok = FALSE;
    if (self->settings->global_hotkey && self->x11_session && self->settings->profile_hotkey) {
        GError *error = NULL;
        self->hotkey_ok = start_hotkeys(self, &error);
        if (!self->hotkey_ok) {
            g_message("global hotkey unavailable: %s", error->message);
            g_error_free(error);
        }
    }
    if (self->settings->profile_hotkey) {
        const gchar *accels[] = {self->settings->profile_hotkey, NULL};
        gtk_application_set_accels_for_action(GTK_APPLICATION(self),
                                              "app.light-next", accels);
    }
    update_tray(self);
}

static void y720_app_shutdown(GApplication *application)
{
    Y720App *self = Y720_APP(application);
    y720_hotkeys_stop();
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
