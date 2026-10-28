/* SPDX-License-Identifier: MIT */
#include "y720-app.h"
#include "y720-window.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "y720-backlight-page.h"
#include "y720-fan-page.h"
#include "y720-settings-dialog.h"

struct _Y720Window {
    AdwApplicationWindow parent_instance;

    Y720Settings *settings;
    gboolean tray_available;
    gboolean hotkey_ok;
    gboolean x11_session;
    gboolean quitting;

    GtkWidget *stack;
    GtkWidget *toast_overlay;
    GtkWidget *fan_page;
    GtkWidget *backlight_page;
    GtkWindow *settings_dialog;
    GtkWidget *background_picture;
    GtkCssProvider *appearance_provider;
    gchar *loaded_background;
};

enum {
    SIGNAL_FAN_STATUS,
    SIGNAL_TOAST,
    SIGNAL_VISIBILITY,
    SIGNAL_PROFILES_CHANGED,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE(Y720Window, y720_window, ADW_TYPE_APPLICATION_WINDOW)

static void on_fan_status(Y720FanPage *page, gpointer user_data)
{
    (void)page;
    g_signal_emit(Y720_WINDOW(user_data), signals[SIGNAL_FAN_STATUS], 0);
}

static void on_fan_toast(Y720FanPage *page, const gchar *message, gpointer user_data)
{
    (void)page;
    y720_window_show_toast(Y720_WINDOW(user_data), message);
}

static void on_curve_changed(Y720FanPage *page, gpointer user_data)
{
    Y720Window *self = Y720_WINDOW(user_data);
    y720_fan_page_get_curve_points(page, self->settings->curve_points);
    y720_settings_save(self->settings);
}

static void on_profiles_changed(Y720BacklightPage *page, gpointer user_data)
{
    (void)page;
    g_signal_emit(Y720_WINDOW(user_data), signals[SIGNAL_PROFILES_CHANGED], 0);
}

static void on_backlight_toast(Y720BacklightPage *page, const gchar *message, gpointer user_data)
{
    (void)page;
    y720_window_show_toast(Y720_WINDOW(user_data), message);
}

static gboolean on_close_request(GtkWindow *window, gpointer user_data)
{
    Y720Window *self = Y720_WINDOW(user_data);
    if (self->quitting || !self->settings->close_to_tray || !self->tray_available)
        return FALSE;

    gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
    return TRUE;
}

static void on_visible_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    Y720Window *self = Y720_WINDOW(user_data);
    gboolean visible = gtk_widget_get_visible(GTK_WIDGET(object));

    (void)visible;
    g_signal_emit(self, signals[SIGNAL_VISIBILITY], 0);
}


static void y720_window_build(Y720Window *self)
{
    gtk_window_set_title(GTK_WINDOW(self), "Y720 Nerve Center");
    gtk_window_set_default_size(GTK_WINDOW(self), 760, 680);
    gtk_widget_set_size_request(GTK_WIDGET(self), 560, 480);
    gtk_window_set_icon_name(GTK_WINDOW(self), "org.y720.Control");

    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);

    self->stack = adw_view_stack_new();
    gtk_widget_set_vexpand(self->stack, TRUE);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), self->stack);

    self->fan_page = y720_fan_page_new();
    y720_fan_page_set_curve_enabled(Y720_FAN_PAGE(self->fan_page),
                                    self->settings->curve_enabled,
                                    self->settings->curve_points);
    self->backlight_page = y720_backlight_page_new();
    adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(self->stack), self->fan_page,
                                        "fan", "极致散热", "power-profile-performance-symbolic");
    adw_view_stack_add_titled_with_icon(ADW_VIEW_STACK(self->stack), self->backlight_page,
                                        "backlight", "幻彩地带", "input-keyboard-symbolic");

    GtkWidget *switcher = adw_view_switcher_new();
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), ADW_VIEW_STACK(self->stack));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), switcher);

    GtkWidget *root = gtk_overlay_new();
    GtkWidget *backdrop = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(backdrop, "window-backdrop");
    gtk_widget_set_hexpand(backdrop, TRUE);
    gtk_widget_set_vexpand(backdrop, TRUE);
    gtk_widget_set_can_target(backdrop, FALSE);
    gtk_overlay_set_child(GTK_OVERLAY(root), backdrop);

    self->background_picture = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(self->background_picture), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_hexpand(self->background_picture, TRUE);
    gtk_widget_set_vexpand(self->background_picture, TRUE);
    gtk_widget_set_halign(self->background_picture, GTK_ALIGN_FILL);
    gtk_widget_set_valign(self->background_picture, GTK_ALIGN_FILL);
    gtk_widget_set_can_target(self->background_picture, FALSE);
    gtk_widget_set_visible(self->background_picture, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(root), self->background_picture);

    self->toast_overlay = adw_toast_overlay_new();
    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(self->toast_overlay), toolbar);
    gtk_widget_set_hexpand(self->toast_overlay, TRUE);
    gtk_widget_set_vexpand(self->toast_overlay, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(root), self->toast_overlay);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), root);

    GMenu *menu = g_menu_new();
    g_menu_append(menu, "设置…", "app.settings");
    g_menu_append(menu, "关于 Y720 Nerve Center", "app.about");
    g_menu_append(menu, "退出", "app.quit");
    GtkWidget *menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu));
    gtk_widget_set_tooltip_text(menu_button, "主菜单");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_button);
    g_object_unref(menu);

    g_signal_connect(self->fan_page, "status-changed", G_CALLBACK(on_fan_status), self);
    g_signal_connect(self->fan_page, "curve-changed", G_CALLBACK(on_curve_changed), self);
    g_signal_connect(self->fan_page, "toast", G_CALLBACK(on_fan_toast), self);
    g_signal_connect(self->backlight_page, "toast", G_CALLBACK(on_backlight_toast), self);
    g_signal_connect(self->backlight_page, "profiles-changed", G_CALLBACK(on_profiles_changed), self);
    g_signal_connect(self, "close-request", G_CALLBACK(on_close_request), self);
    g_signal_connect(self, "notify::visible", G_CALLBACK(on_visible_changed), self);

    GtkEventController *keys = gtk_shortcut_controller_new();
    gtk_shortcut_controller_set_scope(GTK_SHORTCUT_CONTROLLER(keys), GTK_SHORTCUT_SCOPE_MANAGED);
    gtk_widget_add_controller(GTK_WIDGET(self), keys);
    y720_window_apply_appearance(self);
}

static void y720_window_init(Y720Window *self)
{
    (void)self;
}

static void y720_window_dispose(GObject *object)
{
    Y720Window *self = Y720_WINDOW(object);
    if (self->appearance_provider) {
        GdkDisplay *display = gdk_display_get_default();
        if (display)
            gtk_style_context_remove_provider_for_display(display,
                GTK_STYLE_PROVIDER(self->appearance_provider));
        g_clear_object(&self->appearance_provider);
    }
    g_clear_pointer(&self->loaded_background, g_free);
    G_OBJECT_CLASS(y720_window_parent_class)->dispose(object);
}

static void y720_window_class_init(Y720WindowClass *klass)
{
    signals[SIGNAL_FAN_STATUS] = g_signal_new("fan-status",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[SIGNAL_TOAST] = g_signal_new("toast",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);
    G_OBJECT_CLASS(klass)->dispose = y720_window_dispose;

    signals[SIGNAL_VISIBILITY] = g_signal_new("visibility-changed",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[SIGNAL_PROFILES_CHANGED] = g_signal_new("profiles-changed",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

Y720Window *y720_window_new(AdwApplication *application, Y720Settings *settings,
                            gboolean tray_available, gboolean hotkey_ok, gboolean x11_session)
{
    Y720Window *window = g_object_new(Y720_TYPE_WINDOW,
                                      "application", application,
                                      NULL);
    window->settings = settings;
    window->tray_available = tray_available;
    window->hotkey_ok = hotkey_ok;
    window->x11_session = x11_session;
    y720_window_build(window);
    return window;
}

void y720_window_start(Y720Window *self)
{
    y720_fan_page_start(Y720_FAN_PAGE(self->fan_page));
    y720_backlight_page_start(Y720_BACKLIGHT_PAGE(self->backlight_page));
}

void y720_window_show_page(Y720Window *self, guint page)
{
    const gchar *name = page == 1 ? "backlight" : "fan";
    adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(self->stack), name);
}

void y720_window_refresh(Y720Window *self)
{
    y720_fan_page_refresh(Y720_FAN_PAGE(self->fan_page));
    y720_backlight_page_refresh(Y720_BACKLIGHT_PAGE(self->backlight_page));
}

void y720_window_set_fan_mode(Y720Window *self, const gchar *mode)
{
    y720_fan_page_curve_stop(Y720_FAN_PAGE(self->fan_page));
    y720_fan_page_set_mode(Y720_FAN_PAGE(self->fan_page), mode);
}

void y720_window_toggle_boost(Y720Window *self)
{
    y720_fan_page_curve_stop(Y720_FAN_PAGE(self->fan_page));
    y720_fan_page_toggle_boost(Y720_FAN_PAGE(self->fan_page));
}

void y720_window_set_curve_enabled(Y720Window *self, gboolean enabled)
{
    y720_fan_page_set_curve_enabled(Y720_FAN_PAGE(self->fan_page), enabled,
                                    self->settings->curve_points);
}

void y720_window_curve_toggle(Y720Window *self)
{
    y720_fan_page_curve_toggle(Y720_FAN_PAGE(self->fan_page));
}

gboolean y720_window_curve_active(Y720Window *self)
{
    return y720_fan_page_curve_active(Y720_FAN_PAGE(self->fan_page));
}

void y720_window_cycle_profile(Y720Window *self)
{
    y720_backlight_page_cycle_profile(Y720_BACKLIGHT_PAGE(self->backlight_page));
}

static void on_curve_setting_changed(gpointer user_data)
{
    Y720Window *self = Y720_WINDOW(user_data);
    y720_window_set_curve_enabled(self, self->settings->curve_enabled);
    Y720App *app = Y720_APP(gtk_window_get_application(GTK_WINDOW(self)));
    if (app)
        y720_app_reload_shortcut(app);
}

static void on_shortcut_changed(gpointer user_data)
{
    Y720Window *self = Y720_WINDOW(user_data);
    Y720App *app = Y720_APP(gtk_window_get_application(GTK_WINDOW(self)));
    if (app)
        y720_app_reload_shortcut(app);
}

static void on_appearance_changed(gpointer user_data)
{
    y720_window_apply_appearance(Y720_WINDOW(user_data));
}

static void on_settings_destroyed(GtkWindow *dialog, gpointer user_data)
{
    Y720Window *self = Y720_WINDOW(user_data);
    if (self->settings_dialog == dialog)
        self->settings_dialog = NULL;
}

void y720_window_show_settings(Y720Window *self)
{
    self->settings_dialog = y720_settings_dialog_show(GTK_WINDOW(self), self->settings,
                                                       self->tray_available, self->hotkey_ok,
                                                       self->x11_session,
                                                       on_appearance_changed, self,
                                                       on_shortcut_changed, self,
                                                       on_curve_setting_changed, self);
    if (self->settings_dialog)
        g_signal_connect(self->settings_dialog, "destroy",
                         G_CALLBACK(on_settings_destroyed), self);
}

void y720_window_show_toast(Y720Window *self, const gchar *message)
{
    if (!message || !*message)
        return;
    AdwToast *toast = adw_toast_new(message);
    adw_toast_set_timeout(toast, 3);
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->toast_overlay), toast);
}

void y720_window_show_about(Y720Window *self)
{
    AdwAboutWindow *about = ADW_ABOUT_WINDOW(adw_about_window_new());
    adw_about_window_set_application_name(about, "Y720 Nerve Center");
    adw_about_window_set_application_icon(about, "org.y720.Control");
    adw_about_window_set_version(about, Y720_VERSION);
    adw_about_window_set_developer_name(about, "Y720 Linux");
    adw_about_window_set_comments(about, "极致散热与幻彩地带控制。");
    adw_about_window_set_website(about,
        "https://github.com/doublepama10/y720_nerve_linux");
    adw_about_window_set_license_type(about, GTK_LICENSE_MIT_X11);
    gtk_widget_add_css_class(GTK_WIDGET(about), "about-opaque");
    gtk_window_set_transient_for(GTK_WINDOW(about), GTK_WINDOW(self));
    gtk_window_set_modal(GTK_WINDOW(about), TRUE);
    gtk_window_present(GTK_WINDOW(about));
}

gboolean y720_window_fan_controls(Y720Window *self)
{
    return y720_fan_page_controls_available(Y720_FAN_PAGE(self->fan_page));
}

const gchar *y720_window_fan_mode(Y720Window *self)
{
    return y720_fan_page_mode(Y720_FAN_PAGE(self->fan_page));
}

gint y720_window_fan_rpm(Y720Window *self)
{
    return y720_fan_page_rpm(Y720_FAN_PAGE(self->fan_page));
}

static gchar *accent_light(const gchar *accent)
{
    GdkRGBA rgba;
    if (!accent || !gdk_rgba_parse(&rgba, accent))
        return g_strdup("#ff8a8a");
    gint r = (gint)((rgba.red * 0.68 + 0.32) * 255);
    gint g = (gint)((rgba.green * 0.68 + 0.32) * 255);
    gint b = (gint)((rgba.blue * 0.68 + 0.32) * 255);
    return g_strdup_printf("#%02x%02x%02x", r, g, b);
}

void y720_window_apply_appearance(Y720Window *self)
{
    Y720Settings *settings = self->settings;
    const gchar *path = settings->background_path;

    if (path && *path) {
        if (g_strcmp0(self->loaded_background, path) != 0) {
            GError *error = NULL;
            gint source_width = 0, source_height = 0;
            GdkTexture *texture = NULL;
            if (gdk_pixbuf_get_file_info(path, &source_width, &source_height)) {
                gdouble scale = MIN(1.0, MIN(1920.0 / MAX(source_width, 1),
                                             1200.0 / MAX(source_height, 1)));
                gint target_width = MAX(1, (gint)(source_width * scale));
                gint target_height = MAX(1, (gint)(source_height * scale));
                GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(path, target_width,
                                                                       target_height, TRUE, &error);
                if (pixbuf) {
                    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
                    texture = gdk_texture_new_for_pixbuf(pixbuf);
                    G_GNUC_END_IGNORE_DEPRECATIONS
                    g_object_unref(pixbuf);
                }
            } else {
                g_set_error(&error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "无法读取图片");
            }
            if (texture) {
                gtk_picture_set_paintable(GTK_PICTURE(self->background_picture),
                                          GDK_PAINTABLE(texture));
                g_object_unref(texture);
                g_free(self->loaded_background);
                self->loaded_background = g_strdup(path);
                gtk_widget_set_visible(self->background_picture, TRUE);
            } else {
                y720_window_show_toast(self, error ? error->message : "无法载入背景图片");
                g_clear_error(&error);
                gtk_widget_set_visible(self->background_picture, FALSE);
                g_clear_pointer(&self->loaded_background, g_free);
            }
        }
    } else {
        gtk_widget_set_visible(self->background_picture, FALSE);
        g_clear_pointer(&self->loaded_background, g_free);
    }
    gtk_widget_set_opacity(self->background_picture, settings->background_opacity);

    GdkRGBA accent_rgba;
    if (!settings->accent || !gdk_rgba_parse(&accent_rgba, settings->accent))
        gdk_rgba_parse(&accent_rgba, "#c01c28");
    gchar *accent = g_strdup_printf("#%02x%02x%02x",
                                    (gint)(CLAMP(accent_rgba.red, 0.0, 1.0) * 255.0 + 0.5),
                                    (gint)(CLAMP(accent_rgba.green, 0.0, 1.0) * 255.0 + 0.5),
                                    (gint)(CLAMP(accent_rgba.blue, 0.0, 1.0) * 255.0 + 0.5));
    gchar *light = accent_light(accent);
    gdouble alpha = CLAMP(settings->panel_opacity, 0.35, 1.0);

    GString *css = g_string_new(NULL);
    g_string_append_printf(css,
        ":root { --accent-bg-color: %s; --accent-fg-color: #ffffff; --accent-color: %s; }\n",
        accent, light);
    gdouble win_alpha = CLAMP(settings->window_opacity, 0.35, 1.0);
    g_string_append_printf(css,
        ".window-backdrop { background-color: rgba(11,11,13,%.3f); "
        "background-image: linear-gradient(135deg, rgba(23,23,29,%.3f) 0%%, "
        "rgba(10,10,12,%.3f) 58%%, rgba(18,11,13,%.3f) 100%%); }\n",
        win_alpha, win_alpha, win_alpha, win_alpha);
    g_string_append_printf(css, "headerbar { background-color: rgba(16,16,19,%.3f); }\n", alpha);
    g_string_append_printf(css, ".boxed-list, .card { background-color: rgba(23,23,27,%.3f); }\n", alpha);
    g_string_append_printf(css, ".action-bar { background-color: rgba(11,11,13,%.3f); }\n", alpha);
    g_string_append_printf(css,
        "viewswitcher button:checked { background-color: color-mix(in srgb, %s 22%%, transparent); color: %s; }\n",
        accent, light);
    g_string_append_printf(css,
        ".mode-pill { background-color: color-mix(in srgb, %s 22%%, transparent); color: %s; }\n",
        accent, light);
    g_string_append_printf(css,
        ".mode-pill.readonly { background-color: rgba(229,165,10,0.2); color: #e5a50a; }\n");
    g_free(light);
    g_free(accent);

    GdkDisplay *display = gdk_display_get_default();
    if (self->appearance_provider)
        gtk_style_context_remove_provider_for_display(display,
            GTK_STYLE_PROVIDER(self->appearance_provider));
    self->appearance_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(self->appearance_provider, css->str);
    gtk_style_context_add_provider_for_display(display,
        GTK_STYLE_PROVIDER(self->appearance_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_string_free(css, TRUE);
}

void y720_window_set_quitting(Y720Window *self)
{
    self->quitting = TRUE;
}

GtkWidget *y720_window_dialog(Y720Window *self)
{
    if (self->settings_dialog && gtk_widget_get_visible(GTK_WIDGET(self->settings_dialog)))
        return GTK_WIDGET(self->settings_dialog);
    return NULL;
}

guint y720_window_profile_count(Y720Window *self)
{
    return y720_backlight_page_profile_count(Y720_BACKLIGHT_PAGE(self->backlight_page));
}

const gchar *y720_window_profile_name(Y720Window *self, guint index)
{
    return y720_backlight_page_profile_name(Y720_BACKLIGHT_PAGE(self->backlight_page), index);
}

guint y720_window_profile_selected(Y720Window *self)
{
    return y720_backlight_page_profile_selected(Y720_BACKLIGHT_PAGE(self->backlight_page));
}

void y720_window_select_profile(Y720Window *self, guint index)
{
    y720_backlight_page_select_profile(Y720_BACKLIGHT_PAGE(self->backlight_page), index);
}
