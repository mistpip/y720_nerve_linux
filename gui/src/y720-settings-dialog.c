/* SPDX-License-Identifier: MIT */
#include "y720-settings-dialog.h"

#include <adwaita.h>

typedef struct {
    Y720Settings *settings;
    GtkWindow *window;
    GtkWidget *banner;
    GtkWidget *autostart_row;
    GtkWidget *close_row;
    GtkWidget *background_row;
    GtkWidget *shortcut_button;
    GtkWidget *boost_button;
    Y720AppearanceChangedFunc changed;
    gpointer changed_data;
    Y720ShortcutChangedFunc shortcut_changed;
    gpointer shortcut_data;
    Y720CurveChangedFunc curve_changed;
    gpointer curve_data;
    gboolean updating;
} Dialog;

static void dialog_free(gpointer data)
{
    g_free(data);
}

static void save_settings(Dialog *dialog)
{
    y720_settings_save(dialog->settings);
}

static void notify_appearance(Dialog *dialog)
{
    if (dialog->changed)
        dialog->changed(dialog->changed_data);
}

static gchar *rgba_hex(const GdkRGBA *rgba)
{
    return g_strdup_printf("#%02x%02x%02x",
                           (gint)(CLAMP(rgba->red, 0.0, 1.0) * 255.0 + 0.5),
                           (gint)(CLAMP(rgba->green, 0.0, 1.0) * 255.0 + 0.5),
                           (gint)(CLAMP(rgba->blue, 0.0, 1.0) * 255.0 + 0.5));
}

static void update_background_row(Dialog *dialog)
{
    const gchar *path = dialog->settings->background_path;
    gchar *name = path && *path ? g_path_get_basename(path) : NULL;
    adw_action_row_set_subtitle(ADW_ACTION_ROW(dialog->background_row),
                                name ? name : "未选择");
    gtk_widget_set_tooltip_text(dialog->background_row, path && *path ? path : NULL);
    g_free(name);
}

static void banner_error(Dialog *dialog, const gchar *message)
{
    adw_banner_set_title(ADW_BANNER(dialog->banner), message);
    adw_banner_set_revealed(ADW_BANNER(dialog->banner), TRUE);
}

static gchar *pretty_accel(const gchar *accel)
{
    guint key = 0;
    GdkModifierType mods = 0;
    if (!accel || !*accel)
        return g_strdup("未设置");
    gtk_accelerator_parse(accel, &key, &mods);
    if (!key)
        return g_strdup(accel);
    return gtk_accelerator_get_label(key, mods);
}

static void update_shortcut_button(Dialog *dialog)
{
    if (dialog->shortcut_button) {
        gchar *pretty = pretty_accel(dialog->settings->profile_hotkey);
        gtk_button_set_label(GTK_BUTTON(dialog->shortcut_button), pretty);
        g_free(pretty);
    }
    if (dialog->boost_button) {
        gchar *pretty = pretty_accel(dialog->settings->boost_hotkey);
        gtk_button_set_label(GTK_BUTTON(dialog->boost_button), pretty);
        g_free(pretty);
    }
}

typedef struct {
    Dialog *dialog;
    GtkWidget *window;
    gboolean boost;
} Capture;

static gboolean capture_key(GtkEventControllerKey *controller, guint keyval,
                            guint keycode, GdkModifierType state, gpointer user_data)
{
    (void)keycode;
    GtkWidget *window = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    Capture *capture = user_data;
    Dialog *dialog = capture->dialog;
    if (keyval == GDK_KEY_Escape) {
        gtk_window_destroy(GTK_WINDOW(window));
        return TRUE;
    }
    if (keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R ||
        keyval == GDK_KEY_Shift_L || keyval == GDK_KEY_Shift_R ||
        keyval == GDK_KEY_Alt_L || keyval == GDK_KEY_Alt_R ||
        keyval == GDK_KEY_Super_L || keyval == GDK_KEY_Super_R)
        return FALSE;

    GdkModifierType mods = state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK |
                                    GDK_ALT_MASK | GDK_SUPER_MASK);
    if (!(mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)))
        return FALSE;

    gchar *accel = gtk_accelerator_name(keyval, mods);
    if (capture->boost) {
        g_free(dialog->settings->boost_hotkey);
        dialog->settings->boost_hotkey = accel;
    } else {
        g_free(dialog->settings->profile_hotkey);
        dialog->settings->profile_hotkey = accel;
    }
    save_settings(dialog);
    update_shortcut_button(dialog);
    if (dialog->shortcut_changed)
        dialog->shortcut_changed(dialog->shortcut_data);
    gtk_window_destroy(GTK_WINDOW(window));
    return TRUE;
}

static void on_edit_shortcut(GtkButton *button, gpointer user_data)
{
    Dialog *dialog = user_data;
    Capture *capture = g_new0(Capture, 1);
    capture->dialog = dialog;
    capture->boost = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "boost"));
    GtkWidget *window = adw_window_new();
    capture->window = window;
    gtk_window_set_title(GTK_WINDOW(window), capture->boost ? "极致散热" : "切换灯光方案");
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(window), dialog->window);
    gtk_window_set_default_size(GTK_WINDOW(window), 360, 170);

    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    GtkWidget *label = gtk_label_new("按下新的快捷键\n需要 Ctrl、Alt 或 Super");
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_widget_set_margin_top(label, 24);
    gtk_widget_set_margin_bottom(label, 24);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), label);
    adw_window_set_content(ADW_WINDOW(window), toolbar);

    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(capture_key), capture);
    gtk_widget_add_controller(window, keys);
    g_object_set_data_full(G_OBJECT(window), "capture", capture, g_free);
    gtk_window_present(GTK_WINDOW(window));
}

static void on_background_chosen(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Dialog *dialog = user_data;
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
    if (!file) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            banner_error(dialog, error->message);
        g_clear_error(&error);
        return;
    }
    gchar *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path)
        return;
    g_free(dialog->settings->background_path);
    dialog->settings->background_path = path;
    save_settings(dialog);
    update_background_row(dialog);
    notify_appearance(dialog);
}

static void on_choose_background(GtkButton *button, gpointer user_data)
{
    (void)button;
    Dialog *dialog = user_data;
    GtkFileDialog *file_dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(file_dialog, "选择背景图片");
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "图片");
    gtk_file_filter_add_pattern(filter, "*.png");
    gtk_file_filter_add_pattern(filter, "*.jpg");
    gtk_file_filter_add_pattern(filter, "*.jpeg");
    gtk_file_filter_add_pattern(filter, "*.webp");
    gtk_file_filter_add_pattern(filter, "*.bmp");
    gtk_file_filter_add_pattern(filter, "*.gif");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(file_dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_open(file_dialog, dialog->window, NULL, on_background_chosen, dialog);
    g_object_unref(filters);
    g_object_unref(filter);
    g_object_unref(file_dialog);
}

static void on_clear_background(GtkButton *button, gpointer user_data)
{
    (void)button;
    Dialog *dialog = user_data;
    g_clear_pointer(&dialog->settings->background_path, g_free);
    save_settings(dialog);
    update_background_row(dialog);
    notify_appearance(dialog);
}

static void on_background_opacity(GtkRange *range, gpointer user_data)
{
    Dialog *dialog = user_data;
    dialog->settings->background_opacity = gtk_range_get_value(range) / 100.0;
    save_settings(dialog);
    notify_appearance(dialog);
}

static void on_panel_opacity(GtkRange *range, gpointer user_data)
{
    Dialog *dialog = user_data;
    dialog->settings->panel_opacity = gtk_range_get_value(range) / 100.0;
    save_settings(dialog);
    notify_appearance(dialog);
}

static void on_window_opacity(GtkRange *range, gpointer user_data)
{
    Dialog *dialog = user_data;
    dialog->settings->window_opacity = gtk_range_get_value(range) / 100.0;
    save_settings(dialog);
    notify_appearance(dialog);
}

static void on_accent_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    Dialog *dialog = user_data;
    const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(object));
    if (!rgba)
        return;
    gchar *hex = rgba_hex(rgba);
    g_free(dialog->settings->accent);
    dialog->settings->accent = hex;
    save_settings(dialog);
    notify_appearance(dialog);
}

static void on_autostart_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    Dialog *dialog = user_data;
    if (dialog->updating)
        return;
    gboolean active = adw_switch_row_get_active(ADW_SWITCH_ROW(object));
    GError *error = NULL;
    if (!y720_autostart_set(active, &error)) {
        banner_error(dialog, error->message);
        g_error_free(error);
        dialog->updating = TRUE;
        adw_switch_row_set_active(ADW_SWITCH_ROW(object), !active);
        dialog->updating = FALSE;
    }
}

static void on_start_hidden_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    Dialog *dialog = user_data;
    if (dialog->updating)
        return;
    dialog->settings->start_hidden = adw_switch_row_get_active(ADW_SWITCH_ROW(object));
    save_settings(dialog);
}

static void on_close_tray_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    Dialog *dialog = user_data;
    if (dialog->updating)
        return;
    dialog->settings->close_to_tray = adw_switch_row_get_active(ADW_SWITCH_ROW(object));
    save_settings(dialog);
}

static void on_curve_switch(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    Dialog *dialog = user_data;
    if (dialog->updating)
        return;
    dialog->settings->curve_enabled = adw_switch_row_get_active(ADW_SWITCH_ROW(object));
    save_settings(dialog);
    if (dialog->curve_changed)
        dialog->curve_changed(dialog->curve_data);
}

static void on_hotkey_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    Dialog *dialog = user_data;
    if (dialog->updating)
        return;
    dialog->settings->global_hotkey = adw_switch_row_get_active(ADW_SWITCH_ROW(object));
    save_settings(dialog);
    if (dialog->shortcut_changed)
        dialog->shortcut_changed(dialog->shortcut_data);
}

GtkWindow *y720_settings_dialog_show(GtkWindow *parent, Y720Settings *settings,
                                     gboolean tray_available, gboolean hotkey_ok,
                                     gboolean x11_session,
                                     Y720AppearanceChangedFunc changed,
                                     gpointer changed_data,
                                     Y720ShortcutChangedFunc shortcut_changed,
                                     gpointer shortcut_data,
                                     Y720CurveChangedFunc curve_changed,
                                     gpointer curve_data)
{
    Dialog *dialog = g_new0(Dialog, 1);
    dialog->settings = settings;
    dialog->changed = changed;
    dialog->changed_data = changed_data;
    dialog->shortcut_changed = shortcut_changed;
    dialog->shortcut_data = shortcut_data;
    dialog->curve_changed = curve_changed;
    dialog->curve_data = curve_data;

    GtkWidget *window = adw_window_new();
    dialog->window = GTK_WINDOW(window);
    gtk_window_set_title(GTK_WINDOW(window), "设置");
    gtk_window_set_default_size(GTK_WINDOW(window), 560, 620);
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(window), parent);
    g_object_set_data_full(G_OBJECT(window), "y720-dialog", dialog, dialog_free);

    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_window_set_content(ADW_WINDOW(window), toolbar);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(content, "dialog-shell");
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), content);

    dialog->banner = adw_banner_new("设置");
    gtk_box_append(GTK_BOX(content), dialog->banner);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(content), scrolled);

    GtkWidget *page = adw_preferences_page_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), page);

    AdwPreferencesGroup *general = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(general, "常规");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), general);

    AdwSwitchRow *autostart = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(autostart), "开机自动启动");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(autostart), "登录时启动");
    adw_switch_row_set_active(autostart, y720_autostart_enabled());
    g_signal_connect(autostart, "notify::active", G_CALLBACK(on_autostart_changed), dialog);
    adw_preferences_group_add(general, GTK_WIDGET(autostart));

    AdwSwitchRow *start_hidden = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(start_hidden), "启动时隐藏窗口");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(start_hidden), "只显示托盘");
    adw_switch_row_set_active(start_hidden, settings->start_hidden);
    g_signal_connect(start_hidden, "notify::active", G_CALLBACK(on_start_hidden_changed), dialog);
    adw_preferences_group_add(general, GTK_WIDGET(start_hidden));

    AdwSwitchRow *close_tray = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(close_tray), "在托盘区显示图标");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(close_tray),
                                tray_available ? "关闭窗口后继续运行" : "无托盘支持");
    adw_switch_row_set_active(close_tray, settings->close_to_tray);
    gtk_widget_set_sensitive(GTK_WIDGET(close_tray), tray_available);
    g_signal_connect(close_tray, "notify::active", G_CALLBACK(on_close_tray_changed), dialog);
    adw_preferences_group_add(general, GTK_WIDGET(close_tray));

    AdwActionRow *shortcut_row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(shortcut_row), "切换灯光方案");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(shortcut_row), "点击后按新组合键");
    dialog->shortcut_button = gtk_button_new();
    gtk_widget_set_valign(dialog->shortcut_button, GTK_ALIGN_CENTER);
    update_shortcut_button(dialog);
    g_signal_connect(dialog->shortcut_button, "clicked", G_CALLBACK(on_edit_shortcut), dialog);
    adw_action_row_add_suffix(shortcut_row, dialog->shortcut_button);
    adw_preferences_group_add(general, GTK_WIDGET(shortcut_row));

    AdwActionRow *boost_row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(boost_row), "极致散热");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(boost_row), "点击后按新组合键");
    dialog->boost_button = gtk_button_new();
    gtk_widget_set_valign(dialog->boost_button, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(dialog->boost_button), "boost", GINT_TO_POINTER(1));
    update_shortcut_button(dialog);
    g_signal_connect(dialog->boost_button, "clicked", G_CALLBACK(on_edit_shortcut), dialog);
    adw_action_row_add_suffix(boost_row, dialog->boost_button);
    adw_preferences_group_add(general, GTK_WIDGET(boost_row));

    AdwSwitchRow *curve = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(curve), "风扇曲线");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(curve), "显示曲线面板与托盘曲线入口");
    adw_switch_row_set_active(curve, settings->curve_enabled);
    g_signal_connect(curve, "notify::active", G_CALLBACK(on_curve_switch), dialog);
    adw_preferences_group_add(general, GTK_WIDGET(curve));

    AdwPreferencesGroup *appearance = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(appearance, "外观");

    AdwActionRow *background = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(background), "背景图片");
    GtkWidget *background_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(background_buttons, GTK_ALIGN_CENTER);
    GtkWidget *choose = gtk_button_new_with_label("选择…");
    GtkWidget *clear = gtk_button_new_from_icon_name("edit-clear-symbolic");
    gtk_widget_set_tooltip_text(clear, "清除背景");
    gtk_box_append(GTK_BOX(background_buttons), choose);
    gtk_box_append(GTK_BOX(background_buttons), clear);
    adw_action_row_add_suffix(background, background_buttons);
    adw_preferences_group_add(appearance, GTK_WIDGET(background));
    dialog->background_row = GTK_WIDGET(background);
    update_background_row(dialog);
    g_signal_connect(choose, "clicked", G_CALLBACK(on_choose_background), dialog);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_background), dialog);

    AdwActionRow *bg_alpha_row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(bg_alpha_row), "背景不透明度");
    GtkWidget *bg_alpha = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_widget_set_size_request(bg_alpha, 220, -1);
    gtk_scale_set_draw_value(GTK_SCALE(bg_alpha), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(bg_alpha), GTK_POS_RIGHT);
    gtk_scale_set_digits(GTK_SCALE(bg_alpha), 0);
    gtk_range_set_value(GTK_RANGE(bg_alpha), settings->background_opacity * 100.0);
    g_signal_connect(bg_alpha, "value-changed", G_CALLBACK(on_background_opacity), dialog);
    adw_action_row_add_suffix(bg_alpha_row, bg_alpha);
    adw_preferences_group_add(appearance, GTK_WIDGET(bg_alpha_row));

    AdwActionRow *panel_alpha_row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(panel_alpha_row), "面板不透明度");
    GtkWidget *panel_alpha = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 35, 100, 1);
    gtk_widget_set_size_request(panel_alpha, 220, -1);
    gtk_scale_set_draw_value(GTK_SCALE(panel_alpha), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(panel_alpha), GTK_POS_RIGHT);
    gtk_scale_set_digits(GTK_SCALE(panel_alpha), 0);
    gtk_range_set_value(GTK_RANGE(panel_alpha), settings->panel_opacity * 100.0);
    g_signal_connect(panel_alpha, "value-changed", G_CALLBACK(on_panel_opacity), dialog);
    adw_action_row_add_suffix(panel_alpha_row, panel_alpha);
    adw_preferences_group_add(appearance, GTK_WIDGET(panel_alpha_row));

    AdwActionRow *window_alpha_row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(window_alpha_row), "窗口不透明度");
    GtkWidget *window_alpha = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 50, 100, 1);
    gtk_widget_set_size_request(window_alpha, 220, -1);
    gtk_scale_set_draw_value(GTK_SCALE(window_alpha), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(window_alpha), GTK_POS_RIGHT);
    gtk_scale_set_digits(GTK_SCALE(window_alpha), 0);
    gtk_range_set_value(GTK_RANGE(window_alpha), settings->window_opacity * 100.0);
    g_signal_connect(window_alpha, "value-changed", G_CALLBACK(on_window_opacity), dialog);
    adw_action_row_add_suffix(window_alpha_row, window_alpha);
    adw_preferences_group_add(appearance, GTK_WIDGET(window_alpha_row));

    AdwActionRow *accent_row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(accent_row), "强调色");
    GtkColorDialog *color_dialog = gtk_color_dialog_new();
    gtk_color_dialog_set_title(color_dialog, "强调色");
    GtkWidget *color_button = gtk_color_dialog_button_new(color_dialog);
    GdkRGBA rgba;
    if (!gdk_rgba_parse(&rgba, settings->accent ? settings->accent : "#c01c28"))
        gdk_rgba_parse(&rgba, "#c01c28");
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(color_button), &rgba);
    gtk_widget_set_size_request(color_button, 48, 30);
    gtk_widget_set_valign(color_button, GTK_ALIGN_CENTER);
    g_signal_connect(color_button, "notify::rgba", G_CALLBACK(on_accent_changed), dialog);
    adw_action_row_add_suffix(accent_row, color_button);
    adw_preferences_group_add(appearance, GTK_WIDGET(accent_row));
    g_object_set_data_full(G_OBJECT(color_button), "color-dialog", color_dialog,
                           g_object_unref);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), appearance);

    AdwPreferencesGroup *keys = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(keys, "快捷键");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), keys);

    AdwSwitchRow *hotkey = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(hotkey), "启用全局快捷键");
    gchar *subtitle;
    if (!x11_session)
        subtitle = g_strdup("仅 X11 会话");
    else if (!hotkey_ok)
        subtitle = g_strdup("XWayland 或组合键被占用");
    else
        subtitle = g_strdup("X11 全局");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(hotkey), subtitle);
    g_free(subtitle);
    adw_switch_row_set_active(hotkey, settings->global_hotkey && x11_session);
    gtk_widget_set_sensitive(GTK_WIDGET(hotkey), x11_session);
    g_signal_connect(hotkey, "notify::active", G_CALLBACK(on_hotkey_changed), dialog);
    adw_preferences_group_add(keys, GTK_WIDGET(hotkey));

    const struct { const gchar *title; const gchar *accel; } shortcuts[] = {
        {"显示 / 隐藏窗口", "托盘图标"},
        {"切换极致散热页面", "Ctrl+1"},
        {"切换幻彩地带页面", "Ctrl+2"},
        {"刷新状态", "Ctrl+R"},
        {"自动模式", "Ctrl+A"},
        {"设置", "Ctrl+,"},
        {"退出", "Ctrl+Q"},
    };
    for (guint i = 0; i < G_N_ELEMENTS(shortcuts); i++) {
        AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), shortcuts[i].title);
        GtkWidget *label = gtk_label_new(shortcuts[i].accel);
        gtk_widget_add_css_class(label, "mono");
        adw_action_row_add_suffix(row, label);
        adw_preferences_group_add(keys, GTK_WIDGET(row));
    }

    AdwPreferencesGroup *about = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(about, "关于");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), about);
    AdwActionRow *version = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(version), "Y720 Nerve Center");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(version), "版本 " Y720_VERSION);
    adw_preferences_group_add(about, GTK_WIDGET(version));

    gtk_window_present(GTK_WINDOW(window));
    return GTK_WINDOW(window);
}
