/* SPDX-License-Identifier: MIT */
#include <adwaita.h>
#include "y720-fan-page.h"

#include "y720-cli.h"
#include "y720-fan-curve.h"
#include "y720-temp.h"
#include "y720-models.h"

struct _Y720FanPage {
    GtkBox parent_instance;

    GCancellable *cancellable;
    guint poll_id;
    gboolean busy;
    gboolean dirty;
    gboolean elevating;
    gboolean syncing;
    gchar **pending_args;
    gboolean started;
    gboolean have_status;

    Y720FanStatus status;
    gchar *selected_mode;

    GtkWidget *banner;
    GtkWidget *banner_label;
    GtkWidget *rpm_labels[2];
    GtkWidget *mode_pill;
    GtkWidget *target_label;
    GtkWidget *device_label;
    GtkWidget *mode_dropdown;
    GtkWidget *custom_row;
    GtkWidget *rpm_scale;
    GtkWidget *apply_button;
    GtkWidget *refresh_button;
    GtkWidget *diag_labels[Y720_FAN_DIAGNOSTIC_COUNT];
    GtkWidget *diag_rows[Y720_FAN_DIAGNOSTIC_COUNT];

    GtkWidget *curve_group;
    GtkWidget *curve_editor;
    GtkWidget *curve_toggle;
    GtkWidget *curve_temp_label;
    GtkWidget *curve_target_label;
    gint curve_points[Y720_FAN_CURVE_POINTS];
    gboolean curve_enabled;
    gboolean curve_active;
    guint curve_timer;
    gint64 curve_last_write;
    gint curve_last_target;
};

enum {
    SIGNAL_STATUS_CHANGED,
    SIGNAL_TOAST,
    SIGNAL_CURVE_CHANGED,
    SIGNAL_CURVE_ACTIVE_CHANGED,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE(Y720FanPage, y720_fan_page, GTK_TYPE_BOX)

static void update_controls(Y720FanPage *self);
static void curve_set_active(Y720FanPage *self, gboolean active);

static void banner_show(Y720FanPage *self, const gchar *title)
{
    if (!title || !*title) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(self->banner), FALSE);
        gtk_widget_set_visible(self->banner, FALSE);
        return;
    }
    gtk_label_set_text(GTK_LABEL(self->banner_label), title);
    gtk_widget_set_visible(self->banner, TRUE);
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->banner), TRUE);
}

static void toast(Y720FanPage *self, const gchar *message)
{
    g_signal_emit(self, signals[SIGNAL_TOAST], 0, message);
}

static const gchar *mode_label(const gchar *mode)
{
    if (g_strcmp0(mode, "auto") == 0)
        return "自动";
    if (g_strcmp0(mode, "boost") == 0)
        return "极致散热";
    if (g_strcmp0(mode, "curve") == 0)
        return "曲线";
    return "只读";
}

static void set_pill(Y720FanPage *self, const gchar *text, gboolean readonly)
{
    gtk_label_set_text(GTK_LABEL(self->mode_pill), text);
    if (readonly)
        gtk_widget_add_css_class(self->mode_pill, "readonly");
    else
        gtk_widget_remove_css_class(self->mode_pill, "readonly");
}

static void update_labels(Y720FanPage *self)
{
    gchar text[64];

    if (!self->have_status) {
        for (guint i = 0; i < 2; i++)
            gtk_label_set_text(GTK_LABEL(self->rpm_labels[i]), "—");
        gtk_label_set_text(GTK_LABEL(self->target_label), "—");
        gtk_label_set_text(GTK_LABEL(self->device_label), "—");
        set_pill(self, "等待状态", TRUE);
        return;
    }

    for (guint i = 0; i < 2; i++) {
        g_snprintf(text, sizeof(text), "%" G_GINT64_FORMAT " RPM", self->status.fan_rpm[i]);
        gtk_label_set_text(GTK_LABEL(self->rpm_labels[i]), text);
    }

    if (!self->status.controls_available) {
        set_pill(self, "只读", TRUE);
        gtk_label_set_text(GTK_LABEL(self->target_label), "—");
        banner_show(self, "只读模式：allow_control=1 后可控制");
    } else {
        set_pill(self, mode_label(self->status.mode), FALSE);
        if (self->status.target_rpm > 0) {
            g_snprintf(text, sizeof(text), "%" G_GINT64_FORMAT " RPM", self->status.target_rpm);
            gtk_label_set_text(GTK_LABEL(self->target_label), text);
        } else if (g_strcmp0(self->status.mode, "auto") == 0 ||
                   g_strcmp0(self->status.mode, "boost") == 0) {
            gtk_label_set_text(GTK_LABEL(self->target_label), "由固件控制");
        } else {
            gtk_label_set_text(GTK_LABEL(self->target_label), "—");
        }
    }

    gchar *device = g_strdup_printf("%s · 驱动 %s", self->status.device ? self->status.device : "—",
                                    self->status.driver_version ? self->status.driver_version : "—");
    gtk_label_set_text(GTK_LABEL(self->device_label), device);
    g_free(device);

    for (guint i = 0; i < Y720_FAN_DIAGNOSTIC_COUNT; i++) {
        if (!self->status.diagnostic_present[i]) {
            gtk_label_set_text(GTK_LABEL(self->diag_labels[i]), "—");
            gtk_widget_set_visible(self->diag_rows[i], FALSE);
            continue;
        }
        gtk_widget_set_visible(self->diag_rows[i], TRUE);
        g_snprintf(text, sizeof(text), "%" G_GINT64_FORMAT, self->status.diagnostic[i]);
        gtk_label_set_text(GTK_LABEL(self->diag_labels[i]), text);
    }
}

static void update_controls(Y720FanPage *self)
{
    gboolean available = self->have_status && self->status.controls_available;
    gboolean idle = !self->busy;

    gtk_widget_set_sensitive(self->mode_dropdown, available && idle);
    gtk_widget_set_sensitive(self->rpm_scale, available && idle &&
                             g_strcmp0(self->selected_mode, "curve") == 0);
    gtk_widget_set_sensitive(self->apply_button, available && idle);
    gtk_widget_set_visible(self->custom_row, g_strcmp0(self->selected_mode, "curve") == 0);
    gtk_widget_set_sensitive(self->refresh_button, idle);
}

static void apply_status_object(Y720FanPage *self, JsonObject *object)
{
    GError *error = NULL;
    if (!y720_fan_status_parse(object, &self->status, &error)) {
        banner_show(self, error->message);
        g_error_free(error);
        return;
    }
    self->have_status = TRUE;

    if (!self->dirty) {
        const gchar *mode = self->status.mode;
        g_free(self->selected_mode);
        self->selected_mode = g_strdup(mode ? mode : "auto");
        self->syncing = TRUE;
        gtk_drop_down_set_selected(GTK_DROP_DOWN(self->mode_dropdown),
                                   g_strcmp0(mode, "boost") == 0 ? 1 :
                                   g_strcmp0(mode, "curve") == 0 ? 2 : 0);
        self->syncing = FALSE;

        if (self->status.target_rpm > 0)
            gtk_range_set_value(GTK_RANGE(self->rpm_scale),
                                CLAMP(self->status.target_rpm, 1500, 5100));
    }

    update_labels(self);
    update_controls(self);
    g_signal_emit(self, signals[SIGNAL_STATUS_CHANGED], 0);
}

static void status_cb(Y720Result *result, gpointer user_data)
{
    Y720FanPage *self = Y720_FAN_PAGE(user_data);
    self->busy = FALSE;

    if (!result->ok) {
        const gchar *hint = y720_cli_error_hint(result->error_code);
        gchar *message = g_strdup_printf("%s%s%s",
                                         hint ? hint : "",
                                         hint && result->error_message ? "\n" : "",
                                         result->error_message ? result->error_message : "");
        banner_show(self, message);
        g_free(message);
        update_controls(self);
        y720_result_free(result);
        return;
    }

    JsonObject *data = result->data;
    if (!data) {
        banner_show(self, "CLI 成功输出缺少 data");
        y720_result_free(result);
        return;
    }

    apply_status_object(self, data);
    self->dirty = FALSE;
    if (self->status.controls_available)
        banner_show(self, NULL);
    y720_result_free(result);
}

static gboolean poll_cb(gpointer user_data)
{
    Y720FanPage *self = Y720_FAN_PAGE(user_data);
    self->poll_id = 0;
    if (!self->started)
        return G_SOURCE_REMOVE;
    if (!self->busy)
        y720_fan_page_refresh(self);
    /* Hidden window only needs coarse tray status; visible UI polls every second. */
    guint interval = gtk_widget_get_mapped(GTK_WIDGET(self)) ? 1000 : 5000;
    self->poll_id = g_timeout_add(interval, poll_cb, self);
    return G_SOURCE_REMOVE;
}

void y720_fan_page_refresh(Y720FanPage *self)
{
    g_return_if_fail(Y720_IS_FAN_PAGE(self));
    if (self->busy)
        return;

    gchar *binary = y720_cli_locate("y720-fan");
    if (!binary) {
        banner_show(self, "未找到 y720-fan；请安装或设置 Y720_FAN_BIN");
        return;
    }
    const gchar *args[] = {"--json", "status", NULL};
    self->busy = TRUE;
    update_controls(self);
    y720_cli_run(binary, args, NULL, 5, self->cancellable, status_cb, self);
    g_free(binary);
}

static void write_cb(Y720Result *result, gpointer user_data)
{
    Y720FanPage *self = Y720_FAN_PAGE(user_data);
    self->busy = FALSE;

    if (!result->ok) {
        if (g_strcmp0(result->error_code, "permission") == 0 && !self->elevating) {
            gchar *binary = y720_cli_locate("y720-fan");
            if (binary && y720_cli_can_elevate(binary) && self->pending_args) {
                self->elevating = TRUE;
                self->busy = TRUE;
                banner_show(self, "需要管理员权限，正在请求 polkit 授权…");
                update_controls(self);
                y720_cli_run_elevated(binary, (const gchar *const *)self->pending_args,
                                      NULL, 60, self->cancellable, write_cb, self);
                g_free(binary);
                y720_result_free(result);
                return;
            }
            g_free(binary);
        }
        self->elevating = FALSE;
        const gchar *hint = y720_cli_error_hint(result->error_code);
        gchar *message = g_strdup_printf("%s%s%s",
                                         hint ? hint : "",
                                         hint && result->error_message ? "\n" : "",
                                         result->error_message ? result->error_message : "");
        banner_show(self, message);
        toast(self, result->error_code ? result->error_code : "写入失败");
        g_free(message);
    } else {
        self->elevating = FALSE;
        JsonObject *data = result->data;
        JsonNode *status_node = data ? json_object_get_member(data, "status") : NULL;
        if (status_node && JSON_NODE_HOLDS_OBJECT(status_node)) {
            apply_status_object(self, json_object_get_object_member(data, "status"));
        }
        banner_show(self, NULL);
        gboolean settled = data && json_object_has_member(data, "settled") &&
                           json_object_get_boolean_member(data, "settled");
        const gchar *toast_text = "请求已提交";
        if (self->pending_args && self->pending_args[0] &&
            g_strcmp0(self->pending_args[0], "--json") == 0 && self->pending_args[1]) {
            if (g_strcmp0(self->pending_args[1], "mode") == 0 && self->pending_args[2]) {
                toast_text = g_strcmp0(self->pending_args[2], "boost") == 0
                                 ? "已开启极致散热" : "已切换为自动模式";
            } else if (g_strcmp0(self->pending_args[1], "set") == 0) {
                toast_text = "自定义转速已保存";
            }
        }
        toast(self, settled ? "已保存并观察到短时收敛" : toast_text);
        self->dirty = FALSE;
    }

    update_labels(self);
    update_controls(self);
    y720_result_free(result);
    y720_fan_page_refresh(self);
}

static void run_write(Y720FanPage *self, const gchar *const *args, guint timeout)
{
    if (self->busy)
        return;
    gchar *binary = y720_cli_locate("y720-fan");
    if (!binary) {
        banner_show(self, "未找到 y720-fan；请安装或设置 Y720_FAN_BIN");
        return;
    }
    self->busy = TRUE;
    g_strfreev(self->pending_args);
    self->pending_args = g_strdupv((gchar **)args);
    update_controls(self);
    y720_cli_run(binary, args, NULL, timeout, self->cancellable, write_cb, self);
    g_free(binary);
}

void y720_fan_page_set_mode(Y720FanPage *self, const gchar *mode)
{
    g_return_if_fail(Y720_IS_FAN_PAGE(self));
    g_return_if_fail(mode != NULL);
    const gchar *args[] = {"--json", "mode", mode, NULL};
    run_write(self, args, 12);
}

void y720_fan_page_toggle_boost(Y720FanPage *self)
{
    g_return_if_fail(Y720_IS_FAN_PAGE(self));
    if (!self->have_status || !self->status.controls_available)
        return;
    y720_fan_page_set_mode(self, g_strcmp0(self->status.mode, "boost") == 0 ? "auto" : "boost");
}

gboolean y720_fan_page_controls_available(Y720FanPage *self)
{
    return self->have_status && self->status.controls_available;
}

const gchar *y720_fan_page_mode(Y720FanPage *self)
{
    return self->have_status ? self->status.mode : NULL;
}

gint y720_fan_page_rpm(Y720FanPage *self)
{
    return self->have_status ? (gint)self->status.fan_rpm[0] : -1;
}

static void on_mode_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    Y720FanPage *self = Y720_FAN_PAGE(user_data);
    if (self->curve_active)
        curve_set_active(self, FALSE);
    if (self->syncing)
        return;
    guint index = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    const gchar *mode = index == 1 ? "boost" : index == 2 ? "curve" : "auto";
    if (g_strcmp0(self->selected_mode, mode) == 0)
        return;
    g_free(self->selected_mode);
    self->selected_mode = g_strdup(mode);
    self->dirty = TRUE;
    update_controls(self);
}

static void on_apply_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    Y720FanPage *self = Y720_FAN_PAGE(user_data);
    if (self->curve_active)
        curve_set_active(self, FALSE);
    if (!self->have_status || !self->status.controls_available)
        return;

    if (g_strcmp0(self->selected_mode, "curve") == 0) {
        gint rpm = (gint)gtk_range_get_value(GTK_RANGE(self->rpm_scale));
        rpm = ((rpm + 99) / 100) * 100;
        gchar value[16];
        g_snprintf(value, sizeof(value), "%d", CLAMP(rpm, 1500, 5100));
        const gchar *args[] = {"--json", "set", "--rpm", value, NULL};
        run_write(self, args, 15);
    } else {
        y720_fan_page_set_mode(self, g_strcmp0(self->selected_mode, "boost") == 0 ? "boost" : "auto");
    }
}

static void on_restore_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    y720_fan_page_set_mode(Y720_FAN_PAGE(user_data), "auto");
}

static void on_refresh_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    y720_fan_page_refresh(Y720_FAN_PAGE(user_data));
}

static void y720_fan_page_dispose(GObject *object)
{
    Y720FanPage *self = Y720_FAN_PAGE(object);
    y720_fan_page_stop(self);
    if (self->curve_timer) {
        g_source_remove(self->curve_timer);
        self->curve_timer = 0;
    }
    if (self->cancellable) {
        g_cancellable_cancel(self->cancellable);
        g_clear_object(&self->cancellable);
    }
    y720_fan_status_clear(&self->status);
    g_clear_pointer(&self->selected_mode, g_free);
    g_strfreev(self->pending_args);
    G_OBJECT_CLASS(y720_fan_page_parent_class)->dispose(object);
}


static gint curve_rpm_for_temperature(Y720FanPage *self, gint temp)
{
    static const gint temps[Y720_FAN_CURVE_POINTS] = {40, 50, 60, 70, 80, 90};
    if (temp <= temps[0])
        return self->curve_points[0];
    if (temp >= temps[Y720_FAN_CURVE_POINTS - 1])
        return self->curve_points[Y720_FAN_CURVE_POINTS - 1];
    for (guint i = 0; i + 1 < Y720_FAN_CURVE_POINTS; i++) {
        if (temp >= temps[i] && temp <= temps[i + 1]) {
            gdouble ratio = (gdouble)(temp - temps[i]) / (temps[i + 1] - temps[i]);
            gdouble rpm = self->curve_points[i] +
                          ratio * (self->curve_points[i + 1] - self->curve_points[i]);
            return (gint)(rpm / 100.0 + 0.5) * 100;
        }
    }
    return self->curve_points[0];
}

static void curve_update_labels(Y720FanPage *self, gint temp, gint target)
{
    if (!self->curve_temp_label)
        return;
    gchar text[32];
    if (temp < 0) {
        gtk_label_set_text(GTK_LABEL(self->curve_temp_label), "—");
        gtk_label_set_text(GTK_LABEL(self->curve_target_label), "—");
        return;
    }
    g_snprintf(text, sizeof(text), "%d °C", temp);
    gtk_label_set_text(GTK_LABEL(self->curve_temp_label), text);
    g_snprintf(text, sizeof(text), "%d RPM", target);
    gtk_label_set_text(GTK_LABEL(self->curve_target_label), text);
}

static void curve_apply(Y720FanPage *self)
{
    if (!self->curve_active || !self->curve_enabled || self->busy ||
        !self->have_status || !self->status.controls_available)
        return;
    gint temp = y720_temp_read_max();
    if (temp < 0) {
        curve_update_labels(self, -1, 0);
        return;
    }
    gint target = CLAMP(curve_rpm_for_temperature(self, temp), 1500, 5100);
    curve_update_labels(self, temp, target);

    gint64 now = g_get_monotonic_time() / 1000;
    if (target == self->curve_last_target && self->curve_last_write > 0)
        return;
    if (self->curve_last_write > 0 && now - self->curve_last_write < 16000)
        return;

    gchar value[16];
    g_snprintf(value, sizeof(value), "%d", target);
    const gchar *args[] = {"--json", "set", "--rpm", value, NULL};
    self->curve_last_target = target;
    self->curve_last_write = now;
    run_write(self, args, 15);
}

static gboolean curve_tick(gpointer user_data)
{
    Y720FanPage *self = Y720_FAN_PAGE(user_data);
    if (!self->curve_active) {
        self->curve_timer = 0;
        return G_SOURCE_REMOVE;
    }
    curve_apply(self);
    return G_SOURCE_CONTINUE;
}

static void curve_set_active(Y720FanPage *self, gboolean active)
{
    if (self->curve_active == active)
        return;
    self->curve_active = active;
    if (active) {
        self->curve_last_target = 0;
        self->curve_last_write = 0;
        if (!self->curve_timer)
            self->curve_timer = g_timeout_add(4000, curve_tick, self);
        gtk_button_set_label(GTK_BUTTON(self->curve_toggle), "停止曲线");
        curve_apply(self);
    } else {
        if (self->curve_timer) {
            g_source_remove(self->curve_timer);
            self->curve_timer = 0;
        }
        gtk_button_set_label(GTK_BUTTON(self->curve_toggle), "启动曲线");
    }
    g_signal_emit(self, signals[SIGNAL_CURVE_ACTIVE_CHANGED], 0);
}

void y720_fan_page_set_curve_enabled(Y720FanPage *self, gboolean enabled, const gint *points)
{
    g_return_if_fail(Y720_IS_FAN_PAGE(self));
    self->curve_enabled = enabled;
    if (points) {
        for (guint i = 0; i < Y720_FAN_CURVE_POINTS; i++)
            self->curve_points[i] = CLAMP(points[i], 1500, 5100);
        if (self->curve_editor)
            y720_fan_curve_set_points(Y720_FAN_CURVE(self->curve_editor), self->curve_points);
    }
    if (self->curve_group)
        gtk_widget_set_visible(self->curve_group, enabled);
    if (!enabled)
        curve_set_active(self, FALSE);
}

void y720_fan_page_get_curve_points(Y720FanPage *self, gint *points)
{
    for (guint i = 0; i < Y720_FAN_CURVE_POINTS; i++)
        points[i] = self->curve_points[i];
}

void y720_fan_page_curve_start(Y720FanPage *self)
{
    if (self->curve_enabled)
        curve_set_active(self, TRUE);
}

void y720_fan_page_curve_stop(Y720FanPage *self)
{
    curve_set_active(self, FALSE);
}

void y720_fan_page_curve_toggle(Y720FanPage *self)
{
    if (!self->curve_enabled)
        return;
    curve_set_active(self, !self->curve_active);
}

gboolean y720_fan_page_curve_active(Y720FanPage *self)
{
    return self->curve_active;
}

static void on_curve_point_changed(gpointer user_data)
{
    Y720FanPage *self = Y720_FAN_PAGE(user_data);
    if (self->curve_editor)
        y720_fan_curve_get_points(Y720_FAN_CURVE(self->curve_editor), self->curve_points);
    g_signal_emit(self, signals[SIGNAL_CURVE_CHANGED], 0);
    if (self->curve_active) {
        self->curve_last_target = 0;
        curve_apply(self);
    }
}

static void on_curve_toggle(GtkButton *button, gpointer user_data)
{
    (void)button;
    y720_fan_page_curve_toggle(Y720_FAN_PAGE(user_data));
}

static GtkWidget *make_value_label(const gchar *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_add_css_class(label, "mono");
    return label;
}

static AdwActionRow *make_row(const gchar *title, GtkWidget *suffix)
{
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (suffix)
        adw_action_row_add_suffix(row, suffix);
    return row;
}

static void y720_fan_page_init(Y720FanPage *self)
{
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 0);
    self->cancellable = g_cancellable_new();
    self->selected_mode = g_strdup("auto");
    const gint curve_defaults[Y720_FAN_CURVE_POINTS] = {1500, 1800, 2400, 3200, 4200, 5100};
    memcpy(self->curve_points, curve_defaults, sizeof(curve_defaults));

    self->banner = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(self->banner), GTK_REVEALER_TRANSITION_TYPE_NONE);
    gtk_revealer_set_transition_duration(GTK_REVEALER(self->banner), 0);
    GtkWidget *banner_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(banner_bar, "error-bar");
    self->banner_label = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(self->banner_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(self->banner_label), 0.0);
    gtk_widget_set_hexpand(self->banner_label, TRUE);
    gtk_box_append(GTK_BOX(banner_bar), self->banner_label);
    gtk_revealer_set_child(GTK_REVEALER(self->banner), banner_bar);
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->banner), FALSE);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(self), scrolled);

    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 640);
    adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 520);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), clamp);

    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(content, "page-content");
    gtk_widget_set_margin_top(content, 18);
    gtk_widget_set_margin_bottom(content, 28);
    gtk_widget_set_margin_start(content, 18);
    gtk_widget_set_margin_end(content, 18);
    adw_clamp_set_child(ADW_CLAMP(clamp), content);
    gtk_box_append(GTK_BOX(content), self->banner);

    AdwPreferencesGroup *status_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(status_group, "状态");
    

    GtkWidget *rpm_box1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(rpm_box1, GTK_ALIGN_CENTER);
    self->rpm_labels[0] = gtk_label_new("—");
    gtk_widget_add_css_class(self->rpm_labels[0], "rpm-value");
    gtk_box_append(GTK_BOX(rpm_box1), self->rpm_labels[0]);
    adw_preferences_group_add(status_group, GTK_WIDGET(make_row("风扇 1", rpm_box1)));

    GtkWidget *rpm_box2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(rpm_box2, GTK_ALIGN_CENTER);
    self->rpm_labels[1] = gtk_label_new("—");
    gtk_widget_add_css_class(self->rpm_labels[1], "rpm-value");
    gtk_box_append(GTK_BOX(rpm_box2), self->rpm_labels[1]);
    adw_preferences_group_add(status_group, GTK_WIDGET(make_row("风扇 2", rpm_box2)));

    self->mode_pill = gtk_label_new("等待状态");
    gtk_widget_add_css_class(self->mode_pill, "mode-pill");
    GtkWidget *mode_wrap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_valign(mode_wrap, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(mode_wrap), self->mode_pill);
    adw_preferences_group_add(status_group, GTK_WIDGET(make_row("模式", mode_wrap)));

    self->target_label = make_value_label("—");
    adw_preferences_group_add(status_group, GTK_WIDGET(make_row("当前目标", self->target_label)));

    self->device_label = gtk_label_new("—");
    gtk_widget_add_css_class(self->device_label, "tiny");
    adw_preferences_group_add(status_group, GTK_WIDGET(make_row("设备", self->device_label)));

    gtk_box_append(GTK_BOX(content), GTK_WIDGET(status_group));

    AdwPreferencesGroup *control_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(control_group, "控制");

    const gchar *mode_options[] = {"自动", "极致散热", "自定义", NULL};
    self->mode_dropdown = gtk_drop_down_new_from_strings(mode_options);
    gtk_widget_set_size_request(self->mode_dropdown, 160, -1);
    gtk_widget_set_valign(self->mode_dropdown, GTK_ALIGN_CENTER);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(self->mode_dropdown), 0);
    g_signal_connect(self->mode_dropdown, "notify::selected", G_CALLBACK(on_mode_changed), self);
    adw_preferences_group_add(control_group, GTK_WIDGET(make_row("模式", self->mode_dropdown)));

    self->rpm_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1500, 5100, 100);
    gtk_widget_set_size_request(self->rpm_scale, 220, -1);
    gtk_scale_set_draw_value(GTK_SCALE(self->rpm_scale), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(self->rpm_scale), GTK_POS_RIGHT);
    gtk_scale_set_digits(GTK_SCALE(self->rpm_scale), 0);
    gtk_range_set_value(GTK_RANGE(self->rpm_scale), 3000);
    for (gint rpm = 1500; rpm <= 5100; rpm += 900) {
        gchar label[16];
        g_snprintf(label, sizeof(label), "%d", rpm);
        gtk_scale_add_mark(GTK_SCALE(self->rpm_scale), rpm, GTK_POS_BOTTOM, label);
    }
    GtkWidget *scale_wrap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign(scale_wrap, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(scale_wrap), self->rpm_scale);
    self->custom_row = GTK_WIDGET(make_row("目标转速", scale_wrap));
    adw_preferences_group_add(control_group, self->custom_row);

    gtk_box_append(GTK_BOX(content), GTK_WIDGET(control_group));

    AdwPreferencesGroup *curve_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    self->curve_group = GTK_WIDGET(curve_group);
    adw_preferences_group_set_title(curve_group, "风扇曲线");
    adw_preferences_group_set_description(curve_group, "拖动节点调整温度对应的目标转速");
    self->curve_editor = y720_fan_curve_new();
    y720_fan_curve_set_points(Y720_FAN_CURVE(self->curve_editor), self->curve_points);
    y720_fan_curve_set_callback(Y720_FAN_CURVE(self->curve_editor),
                                on_curve_point_changed, self);
    GtkWidget *curve_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(curve_box, 8);
    gtk_widget_set_margin_bottom(curve_box, 8);
    gtk_widget_set_halign(curve_box, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(curve_box, 560, 206);
    gtk_widget_set_hexpand(curve_box, TRUE);
    gtk_widget_set_vexpand(curve_box, FALSE);
    gtk_widget_set_hexpand(self->curve_editor, TRUE);
    gtk_box_append(GTK_BOX(curve_box), self->curve_editor);
    GtkWidget *curve_row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(curve_row), curve_box);
    adw_preferences_group_add(curve_group, curve_row);

    self->curve_temp_label = make_value_label("—");
    adw_preferences_group_add(curve_group, GTK_WIDGET(make_row("温度", self->curve_temp_label)));
    self->curve_target_label = make_value_label("—");
    adw_preferences_group_add(curve_group, GTK_WIDGET(make_row("曲线目标", self->curve_target_label)));

    self->curve_toggle = gtk_button_new_with_label("启动曲线");
    g_signal_connect(self->curve_toggle, "clicked", G_CALLBACK(on_curve_toggle), self);
    adw_preferences_group_add(curve_group, GTK_WIDGET(make_row("自动控制", self->curve_toggle)));
    gtk_widget_set_visible(self->curve_group, FALSE);
    gtk_box_append(GTK_BOX(content), self->curve_group);

    self->refresh_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(self->refresh_button, "刷新状态 (Ctrl+R)");
    g_signal_connect(self->refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), self);
    self->apply_button = gtk_button_new_with_label("保存");
    gtk_widget_add_css_class(self->apply_button, "suggested-action");
    g_signal_connect(self->apply_button, "clicked", G_CALLBACK(on_apply_clicked), self);
    GtkWidget *restore = gtk_button_new_with_label("恢复默认");
    g_signal_connect(restore, "clicked", G_CALLBACK(on_restore_clicked), self);
    AdwExpanderRow *diag = ADW_EXPANDER_ROW(adw_expander_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(diag), "诊断");
    adw_expander_row_set_subtitle(diag, "驱动计数器与错误码");
    for (guint i = 0; i < Y720_FAN_DIAGNOSTIC_COUNT; i++) {
        AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      y720_fan_diagnostic_names[i]);
        self->diag_labels[i] = make_value_label("—");
        adw_action_row_add_suffix(row, self->diag_labels[i]);
        adw_expander_row_add_row(diag, GTK_WIDGET(row));
        self->diag_rows[i] = GTK_WIDGET(row);
    }
    AdwPreferencesGroup *diag_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_add(diag_group, GTK_WIDGET(diag));
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(diag_group));

    GtkWidget *action_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(action_bar, "action-bar");
    gtk_widget_set_halign(action_bar, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(action_bar), restore);
    gtk_box_append(GTK_BOX(action_bar), self->refresh_button);
    gtk_box_append(GTK_BOX(action_bar), self->apply_button);
    gtk_box_append(GTK_BOX(self), action_bar);

    update_labels(self);
    update_controls(self);
}

static void y720_fan_page_class_init(Y720FanPageClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = y720_fan_page_dispose;

    signals[SIGNAL_STATUS_CHANGED] = g_signal_new("status-changed",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);
    signals[SIGNAL_TOAST] = g_signal_new("toast",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);
    signals[SIGNAL_CURVE_CHANGED] = g_signal_new("curve-changed",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);
    signals[SIGNAL_CURVE_ACTIVE_CHANGED] = g_signal_new("curve-active-changed",
        G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);
}

GtkWidget *y720_fan_page_new(void)
{
    return g_object_new(Y720_TYPE_FAN_PAGE, NULL);
}

void y720_fan_page_start(Y720FanPage *self)
{
    g_return_if_fail(Y720_IS_FAN_PAGE(self));
    if (self->started)
        return;
    self->started = TRUE;
    y720_fan_page_refresh(self);
    self->poll_id = g_timeout_add(1000, poll_cb, self);
}

void y720_fan_page_stop(Y720FanPage *self)
{
    g_return_if_fail(Y720_IS_FAN_PAGE(self));
    self->started = FALSE;
    if (self->poll_id) {
        g_source_remove(self->poll_id);
        self->poll_id = 0;
    }
}
