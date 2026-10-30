/* SPDX-License-Identifier: MIT */
#include "y720-backlight-page.h"
#include "y720-i18n.h"

#include <adwaita.h>
#include <math.h>
#include <string.h>

#include "y720-cli.h"
#include "y720-keyboard.h"
#include "y720-profiles.h"

struct _Y720BacklightPage {
    GtkBox parent_instance;

    GCancellable *cancellable;
    gboolean busy;
    gboolean elevating;
    gchar *pending_json;
    gboolean have_info;
    gboolean loading;
    Y720BacklightInfo info;
    Y720BacklightProfile profile;
    Y720ProfileStore *profiles;
    guint profile_index;
    gint selected_segment;
    gboolean is_off;
    guint profile_switch_id;
    guint pending_profile;

    GtkWidget *banner;
    GtkWidget *profile_dropdown;
    GtkWidget *profile_add;
    GtkWidget *profile_remove;
    GtkWidget *profile_rename;
    GtkWidget *keyboard;
    GtkWidget *color_dropdown;
    GtkWidget *swatch;
    GtkWidget *brightness;
    GtkWidget *effect_dropdown;
    GtkWidget *apply_button;
};

enum {
    SIGNAL_TOAST,
    SIGNAL_PROFILE_APPLIED,
    SIGNAL_PROFILES_CHANGED,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE(Y720BacklightPage, y720_backlight_page, GTK_TYPE_BOX)

static void toast(Y720BacklightPage *self, const gchar *message)
{
    g_signal_emit(self, signals[SIGNAL_TOAST], 0, y720_tr(message));
}

static const gchar *effect_list[] = {"呼吸", "心跳", "平滑", "波浪", "常亮", NULL};
static const gint effect_order[5] = {1, 0, 2, 4, 3};

static gint effect_position(gint effect_index)
{
    for (gint i = 0; i < 5; i++)
        if (effect_order[i] == effect_index)
            return i;
    return 4;
}

static const gchar *color_labels[20] = {
    "红", "火炬红", "粉", "洋红", "紫", "灰", "蓝", "深蓝", "浅蓝", "青",
    "深绿", "亮绿", "绿", "淡绿", "橄榄", "黄", "橙", "深橙", "白", "无",
};

static void color_rgba(gint index, GdkRGBA *rgba)
{
    static const gchar *hex[20] = {
        "#e01b24", "#ff5a36", "#ff6fa5", "#ff2bd6", "#a55eea",
        "#77767b", "#3584e4", "#1c71d8", "#62a0ea", "#33d6e0",
        "#26a269", "#2ec27e", "#33d17a", "#8ff0a4", "#c0bf6b",
        "#f6d32d", "#ff7800", "#e66100", "#f6f5f4", "#000000",
    };
    gdk_rgba_parse(rgba, hex[CLAMP(index, 0, 19)]);
    if (index == 19)
        rgba->alpha = 0.0;
}

static void draw_swatch(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data)
{
    (void)user_data;
    gint index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), "color-index"));
    GdkRGBA rgba;
    color_rgba(index, &rgba);

    double radius = 5.0;
    double x = 0.5, y = 0.5, w = width - 1.0, h = height - 1.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - radius, y + radius, radius, -G_PI / 2, 0);
    cairo_arc(cr, x + w - radius, y + h - radius, radius, 0, G_PI / 2);
    cairo_arc(cr, x + radius, y + h - radius, radius, G_PI / 2, G_PI);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);

    if (index == 19) {
        cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.15);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.55);
        cairo_set_line_width(cr, 1.5);
        cairo_move_to(cr, 4, height - 4);
        cairo_line_to(cr, width - 4, 4);
        cairo_stroke(cr);
    } else {
        cairo_set_source_rgba(cr, rgba.red, rgba.green, rgba.blue, 1.0);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.18);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
    }
}

static void set_banner(Y720BacklightPage *self, const gchar *text)
{
    adw_banner_set_title(ADW_BANNER(self->banner), y720_tr(text));
    adw_banner_set_revealed(ADW_BANNER(self->banner), TRUE);
}

static void update_controls(Y720BacklightPage *self)
{
    gboolean idle = !self->busy;
    gtk_widget_set_sensitive(self->apply_button, idle);
    gtk_widget_set_sensitive(self->effect_dropdown, idle && !self->is_off);
    gtk_widget_set_sensitive(self->color_dropdown, idle && !self->is_off);
    gtk_widget_set_sensitive(self->brightness, idle && !self->is_off);
    gtk_widget_set_sensitive(self->profile_dropdown, idle);
    gtk_widget_set_sensitive(self->profile_add, idle);
    gtk_widget_set_sensitive(self->profile_remove,
                             idle && !self->is_off && y720_profiles_count(self->profiles) > 1);
    gtk_widget_set_sensitive(self->profile_rename, idle && !self->is_off);
}

static void update_keyboard(Y720BacklightPage *self)
{
    y720_keyboard_set_profile(self->keyboard, self->profile.colors,
                              self->profile.brightness, self->selected_segment);
}

static void update_swatch(Y720BacklightPage *self)
{
    gtk_widget_queue_draw(self->swatch);
}

static void update_segment_controls(Y720BacklightPage *self)
{
    self->loading = TRUE;
    gint color = self->profile.colors[self->selected_segment];
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->color_dropdown), color);
    gtk_range_set_value(GTK_RANGE(self->brightness),
                        self->profile.brightness[self->selected_segment]);
    self->loading = FALSE;
    g_object_set_data(G_OBJECT(self->swatch), "color-index", GINT_TO_POINTER(color));
    update_swatch(self);
    update_keyboard(self);
}

static void update_editor(Y720BacklightPage *self)
{
    self->loading = TRUE;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->effect_dropdown),
                               effect_position(y720_effect_index(self->profile.effect)));
    self->loading = FALSE;
    update_segment_controls(self);
}

static void store_current_profile(Y720BacklightPage *self)
{
    y720_profiles_set(self->profiles, self->profile_index, &self->profile);
    y720_profiles_save(self->profiles);
}

static void rebuild_profile_dropdown(Y720BacklightPage *self)
{
    guint count = y720_profiles_count(self->profiles);
    const gchar **items = g_new0(const gchar *, count + 1);
    for (guint i = 0; i < count; i++)
        items[i] = y720_profiles_display_name(self->profiles, i);
    self->loading = TRUE;
    GtkStringList *list = gtk_string_list_new(items);
    gtk_drop_down_set_model(GTK_DROP_DOWN(self->profile_dropdown), G_LIST_MODEL(list));
    g_object_unref(list);
    g_free(items);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->profile_dropdown),
                               y720_profiles_selected(self->profiles));
    self->loading = FALSE;
    gtk_widget_set_sensitive(self->profile_remove,
                             !self->is_off && y720_profiles_count(self->profiles) > 1);
    g_signal_emit(self, signals[SIGNAL_PROFILES_CHANGED], 0);
}

static void load_profile(Y720BacklightPage *self, guint index, gboolean apply)
{
    y720_profiles_select(self->profiles, index);
    self->profile_index = index;
    self->is_off = y720_profiles_is_off(self->profiles, index);
    y720_backlight_profile_clear(&self->profile);
    y720_profiles_get(self->profiles, index, &self->profile);
    self->selected_segment = 0;
    update_editor(self);
    rebuild_profile_dropdown(self);
    if (apply)
        y720_backlight_page_apply_current(self);
}

static void refresh_cb(Y720Result *result, gpointer user_data)
{
    Y720BacklightPage *self = Y720_BACKLIGHT_PAGE(user_data);
    self->busy = FALSE;

    if (!result->ok) {
        if (g_strcmp0(result->error_code, "permission") == 0 && !self->elevating) {
            gchar *binary = y720_cli_locate("y720-backlight");
            if (binary && y720_cli_can_elevate(binary)) {
                self->elevating = TRUE;
                self->busy = TRUE;
                set_banner(self, "正在读取设备…");
                update_controls(self);
                const gchar *args[] = {"--json", "status", NULL};
                y720_cli_run_elevated(binary, args, NULL, 30,
                                      self->cancellable, G_OBJECT(self), refresh_cb, self);
                g_free(binary);
                y720_result_free(result);
                return;
            }
            g_free(binary);
        }
        self->elevating = FALSE;
        const gchar *hint = y720_cli_error_hint(result->error_code);
        gchar *message = g_strdup(hint ? hint :
                                  (result->error_message ? result->error_message : "无法读取背光状态"));
        set_banner(self, message);
        g_free(message);
        update_controls(self);
        y720_result_free(result);
        return;
    }
    self->elevating = FALSE;

    GError *error = NULL;
    if (!y720_backlight_info_parse(result->data, &self->info, &error)) {
        self->have_info = FALSE;
        set_banner(self, error->message);
        g_error_free(error);
    } else {
        self->have_info = TRUE;
    }
    update_controls(self);
    y720_result_free(result);
}

void y720_backlight_page_refresh(Y720BacklightPage *self)
{
    g_return_if_fail(Y720_IS_BACKLIGHT_PAGE(self));
    if (self->busy)
        return;
    gchar *binary = y720_cli_locate("y720-backlight");
    if (!binary) {
        set_banner(self, "未找到 y720-backlight");
        return;
    }
    const gchar *args[] = {"--json", "status", NULL};
    self->busy = TRUE;
    update_controls(self);
    y720_cli_run(binary, args, NULL, 5, self->cancellable, G_OBJECT(self), refresh_cb, self);
    g_free(binary);
}

static void apply_cb(Y720Result *result, gpointer user_data)
{
    Y720BacklightPage *self = Y720_BACKLIGHT_PAGE(user_data);
    self->busy = FALSE;

    if (result->ok) {
        JsonNode *completed = result->data ? json_object_get_member(result->data, "reports_completed") : NULL;
        JsonNode *accepted = result->data ? json_object_get_member(result->data, "transport_accepted") : NULL;
        if (!completed || !JSON_NODE_HOLDS_VALUE(completed) ||
            json_node_get_value_type(completed) != G_TYPE_INT64 || json_node_get_int(completed) != 5 ||
            !accepted || !JSON_NODE_HOLDS_VALUE(accepted) ||
            json_node_get_value_type(accepted) != G_TYPE_BOOLEAN || !json_node_get_boolean(accepted)) {
            result->ok = FALSE;
            result->error_code = g_strdup("invalid_output");
            result->error_message = g_strdup("背光回执未确认五条报告已提交，硬件状态未知");
        }
    }

    if (!result->ok) {
        if (g_strcmp0(result->error_code, "permission") == 0 && !self->elevating) {
            gchar *binary = y720_cli_locate("y720-backlight");
            if (binary && y720_cli_can_elevate(binary) && self->pending_json) {
                self->elevating = TRUE;
                self->busy = TRUE;
                set_banner(self, "正在请求授权…");
                update_controls(self);
                const gchar *args[] = {"--json", "apply", "-", NULL};
                y720_cli_run_elevated(binary, args, self->pending_json, 60,
                                      self->cancellable, G_OBJECT(self), apply_cb, self);
                g_free(binary);
                y720_result_free(result);
                return;
            }
            g_free(binary);
        }
        self->elevating = FALSE;
        gint completed = -1;
        gboolean uncertain = FALSE;
        if (result->error_details) {
            if (json_object_has_member(result->error_details, "reports_completed"))
                completed = (gint)json_object_get_int_member(result->error_details, "reports_completed");
            if (json_object_has_member(result->error_details, "state_uncertain"))
                uncertain = json_object_get_boolean_member(result->error_details, "state_uncertain");
        }
        const gchar *hint = y720_cli_error_hint(result->error_code);
        gchar *detail = NULL;
        if (completed >= 0 || uncertain) {
            detail = g_strdup_printf(y720_tr("已发送 %d/5 条%s"), completed < 0 ? 0 : completed,
                                     uncertain ? y720_tr("，状态不确定") : "");
        }
        gchar *message = g_strdup_printf("%s%s%s",
                                         hint ? hint :
                                         (result->error_message ? result->error_message : "写入失败"),
                                         detail ? "\n" : "", detail ? detail : "");
        set_banner(self, message);
        toast(self, "写入失败");
        g_free(detail);
        g_free(message);
        update_controls(self);
        y720_result_free(result);
        return;
    }

    self->elevating = FALSE;
    set_banner(self, "已提交，未读回");
    toast(self, "方案已应用");
    store_current_profile(self);
    g_signal_emit(self, signals[SIGNAL_PROFILE_APPLIED], 0);
    update_controls(self);
    y720_result_free(result);
}

void y720_backlight_page_apply_current(Y720BacklightPage *self)
{
    g_return_if_fail(Y720_IS_BACKLIGHT_PAGE(self));
    if (self->busy)
        return;
    gchar *binary = y720_cli_locate("y720-backlight");
    if (!binary) {
        set_banner(self, "未找到 y720-backlight");
        return;
    }
    gchar *json = y720_backlight_profile_to_json(&self->profile);
    const gchar *args[] = {"--json", "apply", "-", NULL};
    self->busy = TRUE;
    g_free(self->pending_json);
    self->pending_json = g_strdup(json);
    update_controls(self);
    y720_cli_run(binary, args, json, 15, self->cancellable, G_OBJECT(self), apply_cb, self);
    g_free(json);
    g_free(binary);
}

static gboolean apply_pending_profile(gpointer user_data)
{
    Y720BacklightPage *self = Y720_BACKLIGHT_PAGE(user_data);
    self->profile_switch_id = 0;
    load_profile(self, self->pending_profile, TRUE);
    return G_SOURCE_REMOVE;
}

static void on_profile_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    Y720BacklightPage *self = Y720_BACKLIGHT_PAGE(user_data);
    if (self->loading)
        return;
    store_current_profile(self);
    self->pending_profile = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    if (self->profile_switch_id)
        g_source_remove(self->profile_switch_id);
    /* Let the dropdown finish its selection before rebuilding its model. */
    self->profile_switch_id = g_idle_add(apply_pending_profile, self);
}

typedef struct {
    Y720BacklightPage *page;
    AdwDialog *dialog;
    GtkWidget *entry;
} RenameData;

static void rename_data_free(gpointer data)
{
    g_free(data);
}

static void close_rename_dialog(GtkButton *button, gpointer user_data)
{
    (void)button;
    RenameData *data = user_data;
    adw_dialog_close(data->dialog);
}

static void apply_rename(GtkButton *button, gpointer user_data)
{
    (void)button;
    RenameData *data = user_data;
    const gchar *name = gtk_editable_get_text(GTK_EDITABLE(data->entry));
    if (y720_profiles_rename(data->page->profiles, data->page->profile_index, name)) {
        y720_profiles_save(data->page->profiles);
        rebuild_profile_dropdown(data->page);
        toast(data->page, "方案已重命名");
        adw_dialog_close(data->dialog);
    }
}

static void on_profile_rename(GtkButton *button, gpointer user_data)
{
    (void)button;
    Y720BacklightPage *self = Y720_BACKLIGHT_PAGE(user_data);
    if (self->is_off)
        return;

    RenameData *data = g_new0(RenameData, 1);
    data->page = self;
    data->dialog = ADW_DIALOG(adw_dialog_new());
    adw_dialog_set_title(data->dialog, "重命名方案");
    g_object_set_data_full(G_OBJECT(data->dialog), "rename-data", data, rename_data_free);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);

    GtkWidget *label = gtk_label_new("方案名称");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), label);
    data->entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(data->entry),
                          y720_profiles_name(self->profiles, self->profile_index));
    gtk_box_append(GTK_BOX(box), data->entry);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("取消");
    GtkWidget *apply = gtk_button_new_with_label("重命名");
    gtk_widget_add_css_class(apply, "suggested-action");
    g_signal_connect(cancel, "clicked", G_CALLBACK(close_rename_dialog), data);
    g_signal_connect(apply, "clicked", G_CALLBACK(apply_rename), data);
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), apply);
    gtk_box_append(GTK_BOX(box), buttons);

    adw_dialog_set_child(data->dialog, box);
    y720_i18n_apply(GTK_WIDGET(data->dialog));
    adw_dialog_present(data->dialog, GTK_WIDGET(self));
}

static void on_profile_add(GtkButton *button, gpointer user_data)
{
    (void)button;
    Y720BacklightPage *self = Y720_BACKLIGHT_PAGE(user_data);
    store_current_profile(self);
    guint index = y720_profiles_add(self->profiles, &self->profile);
    y720_profiles_save(self->profiles);
    load_profile(self, index, FALSE);
}

static void on_profile_remove(GtkButton *button, gpointer user_data)
{
    (void)button;
    Y720BacklightPage *self = Y720_BACKLIGHT_PAGE(user_data);
    if (y720_profiles_count(self->profiles) <= 1)
        return;
    guint index = self->profile_index;
    if (!y720_profiles_remove(self->profiles, index))
        return;
    y720_profiles_save(self->profiles);
    guint next = MIN(index, y720_profiles_count(self->profiles) - 1);
    load_profile(self, next, TRUE);
}

static void on_keyboard_selected(gint segment, gpointer user_data)
{
    Y720BacklightPage *self = Y720_BACKLIGHT_PAGE(user_data);
    self->selected_segment = segment;
    update_segment_controls(self);
}

static void on_color_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    Y720BacklightPage *self = Y720_BACKLIGHT_PAGE(user_data);
    if (self->loading || self->is_off)
        return;
    guint color = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    self->profile.colors[self->selected_segment] = (gint)color;
    g_object_set_data(G_OBJECT(self->swatch), "color-index", GINT_TO_POINTER(color));
    update_swatch(self);
    update_keyboard(self);
}

static void on_brightness_changed(GtkRange *range, gpointer user_data)
{
    Y720BacklightPage *self = Y720_BACKLIGHT_PAGE(user_data);
    if (self->loading || self->is_off)
        return;
    self->profile.brightness[self->selected_segment] =
        (gint)round(gtk_range_get_value(range));
    update_keyboard(self);
}

static void on_effect_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    Y720BacklightPage *self = Y720_BACKLIGHT_PAGE(user_data);
    if (self->loading || self->is_off)
        return;
    guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    g_free(self->profile.effect);
    self->profile.effect = g_strdup(y720_effect_name(effect_order[selected]));
}

static void on_apply_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    y720_backlight_page_apply_current(Y720_BACKLIGHT_PAGE(user_data));
}

static void on_restore_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    Y720BacklightPage *self = Y720_BACKLIGHT_PAGE(user_data);
    if (self->is_off)
        return;
    y720_backlight_profile_clear(&self->profile);
    self->profile.effect = g_strdup("static");
    for (guint i = 0; i < 4; i++) {
        self->profile.colors[i] = 18;
        self->profile.brightness[i] = 2;
    }
    self->selected_segment = 0;
    update_editor(self);
    y720_backlight_page_apply_current(self);
}

static GtkWidget *make_swatch(gint index)
{
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, 20, 20);
    gtk_widget_set_valign(area, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(area), "color-index", GINT_TO_POINTER(index));
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw_swatch,
                                   GINT_TO_POINTER(index), NULL);
    return area;
}

static GtkWidget *make_color_dropdown(gint selected)
{
    const gchar *items[21] = {0};
    static gchar fallback[20][16];
    for (gint i = 0; i < 20; i++) {
        if (color_labels[i]) {
            items[i] = y720_tr(color_labels[i]);
        } else {
            g_snprintf(fallback[i], sizeof(fallback[i]), "颜色 %d", i);
            items[i] = fallback[i];
        }
    }
    GtkStringList *list = gtk_string_list_new(items);
    GtkWidget *dropdown = gtk_drop_down_new(G_LIST_MODEL(list), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dropdown), selected);
    gtk_widget_set_size_request(dropdown, 112, -1);
    /* gtk_drop_down_new takes full ownership of the model. */
    return dropdown;
}

static AdwActionRow *make_row(const gchar *title, GtkWidget *suffix)
{
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (suffix)
        adw_action_row_add_suffix(row, suffix);
    return row;
}

static void y720_backlight_page_dispose(GObject *object)
{
    Y720BacklightPage *self = Y720_BACKLIGHT_PAGE(object);
    self->loading = TRUE;
    if (self->profile_switch_id) {
        g_source_remove(self->profile_switch_id);
        self->profile_switch_id = 0;
    }
    if (self->cancellable) {
        g_cancellable_cancel(self->cancellable);
        g_clear_object(&self->cancellable);
    }
    y720_backlight_info_clear(&self->info);
    y720_backlight_profile_clear(&self->profile);
    g_clear_pointer(&self->pending_json, g_free);
    g_clear_pointer(&self->profiles, y720_profiles_free);
    G_OBJECT_CLASS(y720_backlight_page_parent_class)->dispose(object);
}

static void y720_backlight_page_init(Y720BacklightPage *self)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    self->cancellable = g_cancellable_new();
    self->profiles = y720_profiles_load();
    self->profile_index = y720_profiles_selected(self->profiles);
    self->selected_segment = 0;
    self->is_off = y720_profiles_is_off(self->profiles, self->profile_index);
    y720_profiles_get(self->profiles, self->profile_index, &self->profile);
    if (!self->profile.effect)
        self->profile.effect = g_strdup("static");

    self->banner = adw_banner_new("");
    adw_banner_set_revealed(ADW_BANNER(self->banner), FALSE);
    gtk_box_append(GTK_BOX(self), self->banner);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(self), scrolled);

    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 660);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 540);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), clamp);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_add_css_class(content, "page-content");
    gtk_widget_set_margin_top(content, 16);
    gtk_widget_set_margin_bottom(content, 24);
    gtk_widget_set_margin_start(content, 16);
    gtk_widget_set_margin_end(content, 16);
    adw_clamp_set_child(ADW_CLAMP(clamp), content);

    AdwPreferencesGroup *profiles_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(profiles_group, "方案");
    GtkWidget *profile_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(profile_box, GTK_ALIGN_CENTER);
    self->profile_dropdown = gtk_drop_down_new(NULL, NULL);
    g_object_set_data(G_OBJECT(self->profile_dropdown), "y720-i18n-user-text", GINT_TO_POINTER(1));
    gtk_widget_set_size_request(self->profile_dropdown, 180, -1);
    self->profile_add = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(self->profile_add, "新建方案");
    self->profile_remove = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_set_tooltip_text(self->profile_remove, "删除方案");
    self->profile_rename = gtk_button_new_from_icon_name("document-edit-symbolic");
    gtk_widget_set_tooltip_text(self->profile_rename, "重命名方案");
    gtk_box_append(GTK_BOX(profile_box), self->profile_dropdown);
    gtk_box_append(GTK_BOX(profile_box), self->profile_add);
    gtk_box_append(GTK_BOX(profile_box), self->profile_rename);
    gtk_box_append(GTK_BOX(profile_box), self->profile_remove);
    adw_preferences_group_add(profiles_group, GTK_WIDGET(make_row("配置文件", profile_box)));
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(profiles_group));
    g_signal_connect(self->profile_dropdown, "notify::selected",
                     G_CALLBACK(on_profile_changed), self);
    g_signal_connect(self->profile_add, "clicked", G_CALLBACK(on_profile_add), self);
    g_signal_connect(self->profile_rename, "clicked", G_CALLBACK(on_profile_rename), self);
    g_signal_connect(self->profile_remove, "clicked", G_CALLBACK(on_profile_remove), self);

    AdwPreferencesGroup *lighting = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(lighting, "幻彩地带");
    adw_preferences_group_set_description(lighting, "点击键盘选择区域");

    self->keyboard = y720_keyboard_new();
    gtk_widget_set_margin_top(self->keyboard, 8);
    gtk_widget_set_margin_bottom(self->keyboard, 8);
    y720_keyboard_set_callback(self->keyboard, on_keyboard_selected, self);
    adw_preferences_group_add(lighting, self->keyboard);

    GtkWidget *color_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(color_box, GTK_ALIGN_CENTER);
    self->swatch = make_swatch(self->profile.colors[0]);
    self->color_dropdown = make_color_dropdown(self->profile.colors[0]);
    self->brightness = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 3, 1);
    gtk_widget_set_size_request(self->brightness, 130, -1);
    gtk_scale_set_draw_value(GTK_SCALE(self->brightness), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(self->brightness), GTK_POS_RIGHT);
    gtk_scale_set_digits(GTK_SCALE(self->brightness), 0);
    gtk_box_append(GTK_BOX(color_box), self->swatch);
    gtk_box_append(GTK_BOX(color_box), self->color_dropdown);
    gtk_box_append(GTK_BOX(color_box), self->brightness);
    adw_preferences_group_add(lighting, GTK_WIDGET(make_row("颜色 / 亮度", color_box)));
    g_signal_connect(self->color_dropdown, "notify::selected",
                     G_CALLBACK(on_color_changed), self);
    g_signal_connect(self->brightness, "value-changed",
                     G_CALLBACK(on_brightness_changed), self);

    const gchar *effects[6];
    for (guint i = 0; i < 5; i++)
        effects[i] = y720_tr(effect_list[i]);
    effects[5] = NULL;
    self->effect_dropdown = gtk_drop_down_new_from_strings(effects);
    gtk_widget_set_size_request(self->effect_dropdown, 130, -1);
    adw_preferences_group_add(lighting, GTK_WIDGET(make_row("灯效", self->effect_dropdown)));
    g_signal_connect(self->effect_dropdown, "notify::selected",
                     G_CALLBACK(on_effect_changed), self);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(lighting));

    GtkWidget *action_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(action_bar, "action-bar");
    gtk_widget_set_halign(action_bar, GTK_ALIGN_END);
    GtkWidget *restore = gtk_button_new_with_label("恢复默认");
    g_signal_connect(restore, "clicked", G_CALLBACK(on_restore_clicked), self);
    self->apply_button = gtk_button_new_with_label("保存");
    gtk_widget_add_css_class(self->apply_button, "suggested-action");
    g_signal_connect(self->apply_button, "clicked", G_CALLBACK(on_apply_clicked), self);
    gtk_box_append(GTK_BOX(action_bar), restore);
    gtk_box_append(GTK_BOX(action_bar), self->apply_button);
    gtk_box_append(GTK_BOX(self), action_bar);

    rebuild_profile_dropdown(self);
    update_editor(self);
    update_controls(self);
}

static void y720_backlight_page_class_init(Y720BacklightPageClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = y720_backlight_page_dispose;

    signals[SIGNAL_TOAST] = g_signal_new("toast",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);
    signals[SIGNAL_PROFILE_APPLIED] = g_signal_new("profile-applied",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);
    signals[SIGNAL_PROFILES_CHANGED] = g_signal_new("profiles-changed",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);
}

GtkWidget *y720_backlight_page_new(void)
{
    return g_object_new(Y720_TYPE_BACKLIGHT_PAGE, NULL);
}

void y720_backlight_page_start(Y720BacklightPage *self)
{
    y720_backlight_page_refresh(self);
}

void y720_backlight_page_cycle_profile(Y720BacklightPage *self)
{
    g_return_if_fail(Y720_IS_BACKLIGHT_PAGE(self));
    if (self->busy)
        return;
    store_current_profile(self);
    guint index = y720_profiles_next(self->profiles);
    y720_profiles_save(self->profiles);
    load_profile(self, index, TRUE);
}

guint y720_backlight_page_profile_count(Y720BacklightPage *self)
{
    return y720_profiles_count(self->profiles);
}

void y720_backlight_page_select_profile(Y720BacklightPage *self, guint index)
{
    g_return_if_fail(Y720_IS_BACKLIGHT_PAGE(self));
    if (self->busy || index >= y720_profiles_count(self->profiles))
        return;
    store_current_profile(self);
    load_profile(self, index, TRUE);
}

const gchar *y720_backlight_page_profile_name(Y720BacklightPage *self, guint index)
{
    return y720_profiles_display_name(self->profiles, index);
}

guint y720_backlight_page_profile_selected(Y720BacklightPage *self)
{
    return self->profile_index;
}
