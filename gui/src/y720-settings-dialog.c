/* SPDX-License-Identifier: MIT */
#include "y720-settings-dialog.h"
#include "y720-evdev.h"
#include "y720-hotkeys.h"
#include <linux/input.h>
#include "y720-i18n.h"

typedef struct {
    Y720Settings *settings;
    GtkWindow *parent;
    GtkWidget *profile_button;
    GtkWidget *boost_button;
    GtkWidget *background_row;
    AdwPreferencesDialog *window;
    Y720AppearanceChangedFunc changed;
    gpointer changed_data;
    Y720ShortcutChangedFunc shortcut_changed;
    gpointer shortcut_data;
    Y720CurveChangedFunc curve_changed;
    gpointer curve_data;
    gboolean updating;
    AdwSwitchRow *global_row;
    guint shortcut_status_timer;
} Dialog;

static gchar *pretty_accel(const gchar *accel)
{
    guint key = 0;
    GdkModifierType mods = 0;

    if (!accel || !*accel)
        return g_strdup(y720_tr("未设置"));
    if (g_str_has_prefix(accel, "keycode:"))
        return g_strdup_printf(y720_tr("键码 %s"), accel + 8);
    if (g_strcmp0(accel, "evdev:228") == 0)
        return g_strdup(y720_tr("Fn+空格"));
    if (g_strcmp0(accel, "evdev:148") == 0)
        return g_strdup(y720_tr("小键盘 7（NumLock 关闭）"));
    if (g_strcmp0(accel, "space") == 0)
        return g_strdup(y720_tr("空格"));
    if (g_str_has_prefix(accel, "evdev:"))
        return g_strdup_printf(y720_tr("键码 %s"), accel + 6);
    gtk_accelerator_parse(accel, &key, &mods);
    if (!key)
        return g_strdup(y720_tr("未设置"));
    return gtk_accelerator_get_label(key, mods);
}

static void update_buttons(Dialog *dialog)
{
    gchar *text;

    text = pretty_accel(dialog->settings->profile_hotkey);
    gtk_button_set_label(GTK_BUTTON(dialog->profile_button), text);
    g_free(text);

    text = pretty_accel(dialog->settings->boost_hotkey);
    gtk_button_set_label(GTK_BUTTON(dialog->boost_button), text);
    g_free(text);
}

typedef struct {
    Dialog *dialog;
    GtkWidget *window;
    gboolean boost;
    guint pending_code;
    guint pending_timeout;
    GObject *dialog_owner;
} Capture;

static void cancel_pending(Capture *capture)
{
    if (capture->pending_timeout) {
        g_source_remove(capture->pending_timeout);
        capture->pending_timeout = 0;
    }
}

static void capture_free(gpointer data)
{
    Capture *capture = data;
    cancel_pending(capture);
    y720_evdev_capture_stop();
    if (capture->dialog->shortcut_changed)
        capture->dialog->shortcut_changed(capture->dialog->shortcut_data);
    g_clear_object(&capture->dialog_owner);
    g_free(capture);
}

static void finish_capture(Capture *capture, const gchar *accel)
{
    Dialog *dialog = capture->dialog;
    cancel_pending(capture);
    if (capture->boost) {
        g_free(dialog->settings->boost_hotkey);
        dialog->settings->boost_hotkey = g_strdup(accel);
    } else {
        g_free(dialog->settings->profile_hotkey);
        dialog->settings->profile_hotkey = g_strdup(accel);
    }
    y720_settings_normalize_shortcuts(dialog->settings);
    y720_settings_save(dialog->settings);
    update_buttons(dialog);
    gtk_window_destroy(GTK_WINDOW(capture->window));
}

static gboolean commit_evdev(gpointer user_data)
{
    Capture *capture = user_data;
    capture->pending_timeout = 0;
    gchar *accel = g_strdup_printf("evdev:%u", capture->pending_code);
    finish_capture(capture, accel);
    g_free(accel);
    return G_SOURCE_REMOVE;
}

static void on_evdev_key(guint code, gpointer user_data)
{
    Capture *capture = user_data;
    if (code == KEY_KBDILLUMTOGGLE) {
        finish_capture(capture, "evdev:228");
        return;
    }
    switch (code) {
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
    case KEY_LEFTALT: case KEY_RIGHTALT:
    case KEY_LEFTMETA: case KEY_RIGHTMETA:
    case KEY_FN:
        return;
    default:
        break;
    }
    capture->pending_code = code;
    cancel_pending(capture);
    capture->pending_timeout = g_timeout_add(80, commit_evdev, capture);
}

static gboolean capture_key(GtkEventControllerKey *controller, guint keyval,
                            guint keycode, GdkModifierType state, gpointer user_data)
{
    GtkWidget *window = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    Capture *capture = user_data;
    GdkModifierType mods = state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK |
                                    GDK_ALT_MASK | GDK_SUPER_MASK);

    if (keyval == GDK_KEY_Escape) {
        gtk_window_destroy(GTK_WINDOW(window));
        return TRUE;
    }
    if (keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R ||
        keyval == GDK_KEY_Shift_L || keyval == GDK_KEY_Shift_R ||
        keyval == GDK_KEY_Alt_L || keyval == GDK_KEY_Alt_R ||
        keyval == GDK_KEY_Super_L || keyval == GDK_KEY_Super_R)
        return FALSE;

    cancel_pending(capture);
    gchar *accel;
    if (keyval == GDK_KEY_KbdLightOnOff ||
        (keyval == GDK_KEY_space && mods == 0 && keycode == 236))
        accel = g_strdup("evdev:228");
    else
        accel = keyval ? gtk_accelerator_name(keyval, mods) :
                         g_strdup_printf("keycode:%u", keycode);

    finish_capture(capture, accel);
    g_free(accel);
    return TRUE;
}

static void on_capture_clicked(GtkButton *button, gpointer user_data)
{
    Dialog *dialog = user_data;
    Capture *capture = g_new0(Capture, 1);
    capture->dialog = dialog;
    capture->dialog_owner = g_object_ref(G_OBJECT(dialog->window));
    capture->boost = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "boost"));

    GtkWidget *window = adw_window_new();
    capture->window = window;
    gtk_window_set_title(GTK_WINDOW(window), capture->boost ? "极致散热" : "切换灯光方案");
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(window), dialog->parent);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(window), 340, 150);

    GtkWidget *toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
    GtkWidget *label = gtk_label_new("按下新的快捷键");
    gtk_widget_set_margin_top(label, 24);
    gtk_widget_set_margin_bottom(label, 24);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), label);
    adw_window_set_content(ADW_WINDOW(window), toolbar);

    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(capture_key), capture);
    gtk_widget_add_controller(window, keys);
    g_object_set_data_full(G_OBJECT(window), "capture", capture, capture_free);
    /* Stop both X11 grabs and Wayland portal actions while recording. */
    y720_hotkeys_stop();
    GError *error = NULL;
    if (!y720_evdev_capture_start(on_evdev_key, capture, &error)) {
        gtk_label_set_text(GTK_LABEL(label), "无法读取 Fn+空格，请检查输入设备权限");
        g_clear_error(&error);
    }
    y720_i18n_apply(window);
    gtk_window_present(GTK_WINDOW(window));
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
        g_warning("autostart: %s", error ? error->message : "unknown error");
        g_clear_error(&error);
        dialog->updating = TRUE;
        adw_switch_row_set_active(ADW_SWITCH_ROW(object), !active);
        dialog->updating = FALSE;
    }
}

static void on_start_hidden_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    Dialog *dialog = user_data;
    if (!dialog->updating) {
        dialog->settings->start_hidden = adw_switch_row_get_active(ADW_SWITCH_ROW(object));
        y720_settings_save(dialog->settings);
    }
}

static void on_close_tray_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    Dialog *dialog = user_data;
    if (!dialog->updating) {
        dialog->settings->close_to_tray = adw_switch_row_get_active(ADW_SWITCH_ROW(object));
        y720_settings_save(dialog->settings);
    }
}

static void on_curve_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    Dialog *dialog = user_data;
    if (!dialog->updating) {
        dialog->settings->curve_enabled = adw_switch_row_get_active(ADW_SWITCH_ROW(object));
        y720_settings_save(dialog->settings);
        if (dialog->curve_changed)
            dialog->curve_changed(dialog->curve_data);
    }
}

static void on_hotkey_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    Dialog *dialog = user_data;
    if (!dialog->updating) {
        dialog->settings->global_hotkey = adw_switch_row_get_active(ADW_SWITCH_ROW(object));
        y720_settings_save(dialog->settings);
        if (dialog->shortcut_changed)
            dialog->shortcut_changed(dialog->shortcut_data);
    }
}

static void notify_appearance(Dialog *dialog)
{
    if (dialog->changed)
        dialog->changed(dialog->changed_data);
}

static void update_background_row(Dialog *dialog)
{
    const gchar *path = dialog->settings->background_path;
    gchar *name = path && *path ? g_path_get_basename(path) : NULL;
    adw_action_row_set_subtitle(ADW_ACTION_ROW(dialog->background_row),
                                name ? name : y720_tr("未选择"));
    gtk_widget_set_tooltip_text(dialog->background_row, path && *path ? path : NULL);
    g_free(name);
}

static void on_background_opacity(GtkRange *range, gpointer user_data)
{
    Dialog *dialog = user_data;
    dialog->settings->background_opacity = gtk_range_get_value(range) / 100.0;
    y720_settings_save(dialog->settings);
    notify_appearance(dialog);
}

static void on_panel_opacity(GtkRange *range, gpointer user_data)
{
    Dialog *dialog = user_data;
    dialog->settings->panel_opacity = gtk_range_get_value(range) / 100.0;
    y720_settings_save(dialog->settings);
    notify_appearance(dialog);
}

static void on_window_opacity(GtkRange *range, gpointer user_data)
{
    Dialog *dialog = user_data;
    dialog->settings->window_opacity = gtk_range_get_value(range) / 100.0;
    y720_settings_save(dialog->settings);
    notify_appearance(dialog);
}

static void on_background_chosen(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Dialog *dialog = user_data;
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
    if (!file) {
        g_clear_error(&error);
        return;
    }
    gchar *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path)
        return;
    g_free(dialog->settings->background_path);
    dialog->settings->background_path = path;
    y720_settings_save(dialog->settings);
    update_background_row(dialog);
    notify_appearance(dialog);
}

static void on_choose_background(GtkButton *button, gpointer user_data)
{
    (void)button;
    Dialog *dialog = user_data;
    GtkFileDialog *file_dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(file_dialog, y720_tr("选择背景图片"));
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, y720_tr("图片"));
    gtk_file_filter_add_pattern(filter, "*.png");
    gtk_file_filter_add_pattern(filter, "*.jpg");
    gtk_file_filter_add_pattern(filter, "*.jpeg");
    gtk_file_filter_add_pattern(filter, "*.webp");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(file_dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_open(file_dialog, dialog->parent, NULL, on_background_chosen, dialog);
    g_object_unref(filters);
    g_object_unref(filter);
    g_object_unref(file_dialog);
}

static void on_clear_background(GtkButton *button, gpointer user_data)
{
    (void)button;
    Dialog *dialog = user_data;
    g_clear_pointer(&dialog->settings->background_path, g_free);
    y720_settings_save(dialog->settings);
    update_background_row(dialog);
    notify_appearance(dialog);
}

static const gchar *language_values[] = {"auto", "zh_CN", "en"};

static void on_language_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    Dialog *dialog = user_data;
    if (dialog->updating)
        return;
    guint index = adw_combo_row_get_selected(ADW_COMBO_ROW(object));
    if (index > 2)
        index = 0;
    g_free(dialog->settings->language);
    dialog->settings->language = g_strdup(language_values[index]);
    y720_settings_save(dialog->settings);
    y720_i18n_set_language(dialog->settings->language);
    AdwToast *toast = adw_toast_new(y720_tr("重启程序后生效"));
    adw_preferences_dialog_add_toast(dialog->window, toast);
}

static void dialog_free(gpointer data)
{
    Dialog *dialog = data;
    if (dialog->shortcut_status_timer)
        g_source_remove(dialog->shortcut_status_timer);
    g_free(dialog);
}

static gboolean update_shortcut_status(gpointer data)
{
    Dialog *dialog = data;
    g_autofree gchar *text = NULL;
    if (!dialog->settings->global_hotkey)
        text = g_strdup(y720_tr("全局快捷键已关闭"));
    else
        text = g_strdup_printf("%s\n%s", y720_hotkeys_status(),
                                y720_tr(y720_evdev_active() ? "Y720 专用功能键可用" :
                                   "Y720 专用功能键不可读，请检查活动会话与输入权限"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(dialog->global_row), text);
    return G_SOURCE_CONTINUE;
}

AdwDialog *y720_settings_dialog_show(GtkWindow *parent, Y720Settings *settings,
                                     gboolean tray_available, gboolean hotkey_ok,
                                     gboolean x11_session,
                                     Y720AppearanceChangedFunc changed,
                                     gpointer changed_data,
                                     Y720ShortcutChangedFunc shortcut_changed,
                                     gpointer shortcut_data,
                                     Y720CurveChangedFunc curve_changed,
                                     gpointer curve_data)
{
    (void)hotkey_ok; (void)x11_session;
    Dialog *dialog = g_new0(Dialog, 1);
    dialog->settings = settings;
    dialog->parent = parent;
    dialog->changed = changed;
    dialog->changed_data = changed_data;
    dialog->shortcut_changed = shortcut_changed;
    dialog->shortcut_data = shortcut_data;
    dialog->curve_changed = curve_changed;
    dialog->curve_data = curve_data;

    AdwPreferencesDialog *prefs = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    dialog->window = prefs;
    adw_dialog_set_title(ADW_DIALOG(prefs), "设置");
    adw_preferences_dialog_set_search_enabled(prefs, FALSE);
    g_object_set_data_full(G_OBJECT(prefs), "y720-dialog", dialog, dialog_free);

    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page, "设置");
    adw_preferences_dialog_add(prefs, page);

    AdwPreferencesGroup *startup = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(startup, "启动");

    AdwSwitchRow *autostart = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(autostart), "开机自动启动");
    adw_switch_row_set_active(autostart, y720_autostart_enabled());
    g_signal_connect(autostart, "notify::active", G_CALLBACK(on_autostart_changed), dialog);
    adw_preferences_group_add(startup, GTK_WIDGET(autostart));

    AdwSwitchRow *hidden = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(hidden), "启动时隐藏窗口");
    adw_switch_row_set_active(hidden, settings->start_hidden);
    g_signal_connect(hidden, "notify::active", G_CALLBACK(on_start_hidden_changed), dialog);
    adw_preferences_group_add(startup, GTK_WIDGET(hidden));

    AdwSwitchRow *tray = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(tray), "关闭窗口后留在托盘");
    adw_switch_row_set_active(tray, settings->close_to_tray);
    gtk_widget_set_sensitive(GTK_WIDGET(tray), tray_available);
    g_signal_connect(tray, "notify::active", G_CALLBACK(on_close_tray_changed), dialog);
    adw_preferences_group_add(startup, GTK_WIDGET(tray));

    AdwComboRow *language = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(language), "语言");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(language), "重启程序后生效");
    const gchar *language_labels[] = {y720_tr("跟随系统"), "中文", "English", NULL};
    GtkStringList *language_list = gtk_string_list_new(language_labels);
    adw_combo_row_set_model(language, G_LIST_MODEL(language_list));
    g_object_unref(language_list);
    guint language_index = g_strcmp0(settings->language, "zh_CN") == 0 ? 1 :
                           g_strcmp0(settings->language, "en") == 0 ? 2 : 0;
    adw_combo_row_set_selected(language, language_index);
    g_signal_connect(language, "notify::selected", G_CALLBACK(on_language_changed), dialog);
    adw_preferences_group_add(startup, GTK_WIDGET(language));

    adw_preferences_page_add(page, startup);

    AdwPreferencesGroup *keys = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(keys, "快捷键");

    AdwActionRow *profile = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(profile), "切换灯光方案");
    dialog->profile_button = gtk_button_new();
    gtk_widget_set_valign(dialog->profile_button, GTK_ALIGN_CENTER);
    g_signal_connect(dialog->profile_button, "clicked", G_CALLBACK(on_capture_clicked), dialog);
    adw_action_row_add_suffix(profile, dialog->profile_button);
    adw_preferences_group_add(keys, GTK_WIDGET(profile));

    AdwActionRow *boost = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(boost), "极致散热");
    dialog->boost_button = gtk_button_new();
    gtk_widget_set_valign(dialog->boost_button, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(dialog->boost_button), "boost", GINT_TO_POINTER(1));
    g_signal_connect(dialog->boost_button, "clicked", G_CALLBACK(on_capture_clicked), dialog);
    adw_action_row_add_suffix(boost, dialog->boost_button);
    adw_preferences_group_add(keys, GTK_WIDGET(boost));

    AdwSwitchRow *global = ADW_SWITCH_ROW(adw_switch_row_new());
    dialog->global_row = global;
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(global), "启用全局快捷键");
    adw_switch_row_set_active(global, settings->global_hotkey);
    update_shortcut_status(dialog);
    dialog->shortcut_status_timer = g_timeout_add(250, update_shortcut_status, dialog);
    g_signal_connect(global, "notify::active", G_CALLBACK(on_hotkey_changed), dialog);
    adw_preferences_group_add(keys, GTK_WIDGET(global));

    adw_preferences_page_add(page, keys);

    AdwPreferencesGroup *fan = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(fan, "风扇");
    AdwSwitchRow *curve = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(curve), "启用风扇曲线");
    adw_switch_row_set_active(curve, settings->curve_enabled);
    g_signal_connect(curve, "notify::active", G_CALLBACK(on_curve_changed), dialog);
    adw_preferences_group_add(fan, GTK_WIDGET(curve));
    adw_preferences_page_add(page, fan);

    AdwPreferencesGroup *appearance = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(appearance, "外观");

    AdwActionRow *background = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(background), "背景图片");
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(buttons, GTK_ALIGN_CENTER);
    GtkWidget *choose = gtk_button_new_with_label("选择…");
    GtkWidget *clear = gtk_button_new_from_icon_name("edit-clear-symbolic");
    gtk_widget_set_tooltip_text(clear, "清除背景");
    gtk_box_append(GTK_BOX(buttons), choose);
    gtk_box_append(GTK_BOX(buttons), clear);
    adw_action_row_add_suffix(background, buttons);
    dialog->background_row = GTK_WIDGET(background);
    update_background_row(dialog);
    g_signal_connect(choose, "clicked", G_CALLBACK(on_choose_background), dialog);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_background), dialog);
    adw_preferences_group_add(appearance, GTK_WIDGET(background));

    struct { const gchar *title; gdouble value; gdouble min; gdouble max; GCallback cb; } sliders[] = {
        {"背景不透明度", settings->background_opacity * 100.0, 0, 100, G_CALLBACK(on_background_opacity)},
        {"面板不透明度", settings->panel_opacity * 100.0, 35, 100, G_CALLBACK(on_panel_opacity)},
        {"窗口不透明度", settings->window_opacity * 100.0, 50, 100, G_CALLBACK(on_window_opacity)},
    };
    for (guint i = 0; i < G_N_ELEMENTS(sliders); i++) {
        AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), sliders[i].title);
        GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                     sliders[i].min, sliders[i].max, 1);
        gtk_widget_set_size_request(scale, 220, -1);
        gtk_scale_set_draw_value(GTK_SCALE(scale), TRUE);
        gtk_scale_set_value_pos(GTK_SCALE(scale), GTK_POS_RIGHT);
        gtk_scale_set_digits(GTK_SCALE(scale), 0);
        gtk_range_set_value(GTK_RANGE(scale), sliders[i].value);
        g_signal_connect(scale, "value-changed", sliders[i].cb, dialog);
        adw_action_row_add_suffix(row, scale);
        adw_preferences_group_add(appearance, GTK_WIDGET(row));
    }
    adw_preferences_page_add(page, appearance);

    update_buttons(dialog);
    y720_i18n_apply(GTK_WIDGET(prefs));
    return ADW_DIALOG(prefs);
}
