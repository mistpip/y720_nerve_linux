/* SPDX-License-Identifier: MIT */
#include <adwaita.h>
#include "y720-fan-page.h"
#include "y720-i18n.h"

#include "y720-cli.h"
#include "y720-fan-curve.h"
#include "y720-temp.h"
#include "y720-models.h"

struct _Y720FanPage {
    GtkBox parent_instance;

    GCancellable *cancellable;
    guint poll_id;
    guint curve_commit_id;
    gboolean busy;
    gboolean refreshing;
    gboolean dirty;
    gboolean elevating;
    gboolean syncing;
    gchar **pending_args;
    gboolean started;
    gboolean have_status;
    guint write_seq;
    guint query_seq;
    gchar *queued_mode;
    gchar *pending_input;
    gint queued_curve;
    gboolean curve_dirty;
    guint curve_generation;
    guint pending_curve_generation;

    Y720FanStatus status;
    gchar *selected_mode;

    GtkWidget *banner;
    GtkWidget *banner_label;
    gchar *banner_text;
    GtkWidget *rpm_labels[2];
    GtkWidget *mode_pill;
    GtkWidget *device_label;
    GtkWidget *mode_toggle;
    GtkWidget *custom_row;
    GtkWidget *manual_group;
    GtkWidget *rpm_scale;
    GtkWidget *apply_button;
    GtkWidget *refresh_button;

    GtkWidget *curve_group;
    GtkWidget *curve_editor;
    GtkWidget *curve_temp_label;
    gint curve_points[Y720_FAN_CURVE_POINTS];
    gboolean curve_enabled;
    gboolean curve_active;
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
    if (g_strcmp0(self->banner_text, title) == 0)
        return;
    g_free(self->banner_text);
    self->banner_text = title && *title ? g_strdup(title) : NULL;
    if (!title || !*title) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(self->banner), FALSE);
        gtk_widget_set_visible(self->banner, FALSE);
        return;
    }
    gtk_label_set_text(GTK_LABEL(self->banner_label), y720_tr(title));
    gtk_widget_set_visible(self->banner, TRUE);
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->banner), TRUE);
}

static void set_sensitive_if_changed(GtkWidget *widget, gboolean sensitive)
{
    if (gtk_widget_get_sensitive(widget) != sensitive)
        gtk_widget_set_sensitive(widget, sensitive);
}

static void set_visible_if_changed(GtkWidget *widget, gboolean visible)
{
    if (gtk_widget_get_visible(widget) != visible)
        gtk_widget_set_visible(widget, visible);
}

static void toast(Y720FanPage *self, const gchar *message)
{
    g_signal_emit(self, signals[SIGNAL_TOAST], 0, y720_tr(message));
}

static const gchar *mode_label(const gchar *mode)
{
    if (g_strcmp0(mode, "auto") == 0)
        return "自动";
    if (g_strcmp0(mode, "boost") == 0)
        return "极致散热";
    if (g_strcmp0(mode, "curve") == 0)
        return "曲线";
    if (g_strcmp0(mode, "custom") == 0)
        return "手动";
    return "只读";
}

static void set_label_if_changed(GtkWidget *label, const gchar *text)
{
    if (g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), text) != 0)
        gtk_label_set_text(GTK_LABEL(label), text);
}

static void set_pill(Y720FanPage *self, const gchar *text, gboolean readonly)
{
    set_label_if_changed(self->mode_pill, y720_tr(text));
    if (g_strcmp0(text, "极致散热") == 0)
        gtk_widget_add_css_class(self->mode_pill, "boost");
    else
        gtk_widget_remove_css_class(self->mode_pill, "boost");
    if (readonly)
        gtk_widget_add_css_class(self->mode_pill, "readonly");
    else
        gtk_widget_remove_css_class(self->mode_pill, "readonly");
}

static void update_labels(Y720FanPage *self)
{
    gchar text[64];

    if (!self->have_status) {
        if (self->curve_temp_label) {
            gtk_label_set_text(GTK_LABEL(self->curve_temp_label), "—");
            gtk_widget_set_tooltip_text(self->curve_temp_label, NULL);
        }
        for (guint i = 0; i < 2; i++)
            gtk_label_set_text(GTK_LABEL(self->rpm_labels[i]), "—");
        set_label_if_changed(self->device_label, "—");
        set_pill(self, "等待状态", TRUE);
        return;
    }

    for (guint i = 0; i < 2; i++) {
        g_snprintf(text, sizeof(text), "%" G_GINT64_FORMAT " RPM", self->status.fan_rpm[i]);
        set_label_if_changed(self->rpm_labels[i], text);
    }

    if (!self->status.controls_available) {
        set_pill(self, "只读", TRUE);
        banner_show(self, "只读模式：allow_control=1 后可控制");
    } else {
        set_pill(self, mode_label(g_strcmp0(self->status.mode, "curve") == 0 && !self->curve_active ?
                                 "custom" : self->status.mode), FALSE);
    }

    set_label_if_changed(self->device_label,
                         self->status.device ? self->status.device : "—");

}

static void update_controls(Y720FanPage *self)
{
    gboolean available = self->have_status && self->status.controls_available &&
                         !g_strcmp0(self->status.driver_version, Y720_VERSION);
    gboolean idle = !self->busy;

    set_sensitive_if_changed(self->mode_toggle, available && idle);
    set_sensitive_if_changed(self->rpm_scale, available && idle &&
                             g_strcmp0(self->selected_mode, "custom") == 0);
    set_sensitive_if_changed(self->apply_button, available && idle);
    gboolean manual = g_strcmp0(self->selected_mode, "custom") == 0;
    set_visible_if_changed(self->manual_group, manual);
    set_visible_if_changed(self->custom_row, manual);
    set_visible_if_changed(self->apply_button, g_strcmp0(self->selected_mode, "custom") == 0);
    set_sensitive_if_changed(self->refresh_button, idle);
}

static void apply_status_object(Y720FanPage *self, JsonObject *object)
{
    GError *error = NULL;
    if (!y720_fan_status_parse(object, &self->status, &error)) {
        self->have_status = FALSE;
        banner_show(self, error->message);
        g_error_free(error);
        update_labels(self);
        update_controls(self);
        g_signal_emit(self, signals[SIGNAL_STATUS_CHANGED], 0);
        return;
    }
    self->have_status = TRUE;
    gboolean was_active = self->curve_active;
    self->curve_active = self->status.kernel_curve_supported && self->status.kernel_curve_active;
    if (was_active != self->curve_active)
        g_signal_emit(self, signals[SIGNAL_CURVE_ACTIVE_CHANGED], 0);
    if (self->curve_active && !self->curve_dirty) {
        memcpy(self->curve_points, self->status.kernel_curve_points, sizeof(self->curve_points));
        y720_fan_curve_set_points(Y720_FAN_CURVE(self->curve_editor), self->curve_points);
    }
    gint temp = y720_temp_display_from_status(&self->status);
    gchar text[32];
    if (temp < 0) g_strlcpy(text, "—", sizeof(text));
    else g_snprintf(text, sizeof(text), "%d °C", temp);
    gtk_label_set_text(GTK_LABEL(self->curve_temp_label), text);
    const gchar *hint = temp < 0 ? "温度读数不可用" : "内核控制温度，来源为 CPU 封装传感器";
    gtk_widget_set_tooltip_text(self->curve_temp_label, y720_tr(hint));
    if (self->curve_active && self->status.kernel_curve_error)
        banner_show(self, "内核曲线温度暂不可用，驱动保留最后目标并重试");
    else if (self->curve_active && self->status.diagnostic_present[2] && self->status.diagnostic[2])
        banner_show(self, "驱动报告散热异常，曲线目标保持不变；详情请查看 CLI 状态");
    else if (self->curve_active) banner_show(self, NULL);
    if (g_strcmp0(self->status.driver_version, Y720_VERSION))
        banner_show(self, "驱动与 GUI 版本不一致，已禁用写入");

    if (!self->dirty) {
        const gchar *mode = self->status.mode;
        g_free(self->selected_mode);
        if (g_strcmp0(mode, "boost") == 0) {
            self->selected_mode = g_strdup("boost");
        } else if (g_strcmp0(mode, "curve") == 0) {
            self->selected_mode = g_strdup(self->curve_active ? "curve" : "custom");
        } else {
            self->selected_mode = g_strdup("auto");
        }
        self->syncing = TRUE;
        gint selected = g_strcmp0(self->selected_mode, "boost") == 0 ? 1 :
                        g_strcmp0(self->selected_mode, "custom") == 0 ? 2 :
                        g_strcmp0(self->selected_mode, "curve") == 0 ? 3 : 0;
        if (adw_toggle_group_get_active(ADW_TOGGLE_GROUP(self->mode_toggle)) != (guint)selected)
            adw_toggle_group_set_active(ADW_TOGGLE_GROUP(self->mode_toggle), selected);
        self->syncing = FALSE;

        if (self->status.target_rpm > 0) {
            gdouble rpm = CLAMP(self->status.target_rpm, 1500, 5100);
            if (gtk_range_get_value(GTK_RANGE(self->rpm_scale)) != rpm)
                gtk_range_set_value(GTK_RANGE(self->rpm_scale), rpm);
        }
    }

    update_labels(self);
    update_controls(self);
    g_signal_emit(self, signals[SIGNAL_STATUS_CHANGED], 0);
}

static void status_cb(Y720Result *result, gpointer user_data)
{
    Y720FanPage *self = Y720_FAN_PAGE(user_data);
    self->refreshing = FALSE;
    if (self->query_seq != self->write_seq) {
        y720_result_free(result);
        return;
    }

    if (!result->ok) {
        self->have_status = FALSE;
        const gchar *hint = y720_cli_error_hint(result->error_code);
        gchar *message = g_strdup(hint ? hint :
                                  (result->error_message ? result->error_message : "无法读取风扇状态"));
        banner_show(self, message);
        g_free(message);
        update_controls(self);
        update_labels(self);
        g_signal_emit(self, signals[SIGNAL_STATUS_CHANGED], 0);
        y720_result_free(result);
        return;
    }

    JsonObject *data = result->data;
    if (!data) {
        self->have_status = FALSE;
        banner_show(self, "CLI 成功输出缺少 data");
        update_labels(self);
        update_controls(self);
        g_signal_emit(self, signals[SIGNAL_STATUS_CHANGED], 0);
        y720_result_free(result);
        return;
    }

    gboolean was_active = self->curve_active;
    apply_status_object(self, data);
    if (was_active && self->have_status && !self->curve_active)
        toast(self, "内核曲线已停止，控制权已交给其他模式或外部控制者");
    if (self->have_status && self->status.controls_available && !self->curve_active &&
        !g_strcmp0(self->status.driver_version, Y720_VERSION))
        banner_show(self, NULL);
    y720_result_free(result);
}

static gboolean poll_cb(gpointer user_data)
{
    Y720FanPage *self = Y720_FAN_PAGE(user_data);
    self->poll_id = 0;
    if (!self->started)
        return G_SOURCE_REMOVE;
    if (!self->busy && !self->refreshing)
        y720_fan_page_refresh(self);
    /* Visible UI needs RPM, but not every frame; hidden window only feeds the tray. */
    guint interval = self->curve_active || gtk_widget_get_mapped(GTK_WIDGET(self)) ? 5 : 15;
    self->poll_id = g_timeout_add_seconds(interval, poll_cb, self);
    return G_SOURCE_REMOVE;
}

void y720_fan_page_refresh(Y720FanPage *self)
{
    g_return_if_fail(Y720_IS_FAN_PAGE(self));
    if (self->busy || self->refreshing)
        return;

    gchar *binary = y720_cli_locate("y720-fan");
    if (!binary) {
        banner_show(self, "未找到 y720-fan；请安装或设置 Y720_FAN_BIN");
        return;
    }
    const gchar *args[] = {"--json", "status", NULL};
    self->refreshing = TRUE;
    self->query_seq = self->write_seq;
    y720_cli_run(binary, args, NULL, 5, self->cancellable, G_OBJECT(self), status_cb, self);
    g_free(binary);
}

static void write_cb(Y720Result *result, gpointer user_data)
{
    Y720FanPage *self = Y720_FAN_PAGE(user_data);
    self->busy = FALSE;

    if (result->ok) {
        JsonNode *node = result->data ? json_object_get_member(result->data, "status") : NULL;
        Y720FanStatus status = {0};
        GError *error = NULL;
        gboolean valid = node && JSON_NODE_HOLDS_OBJECT(node) &&
                         y720_fan_status_parse(json_node_get_object(node), &status, &error);
        if (!valid || !status.controls_available || g_strcmp0(status.driver_version, Y720_VERSION)) {
            result->ok = FALSE;
            result->error_code = g_strdup("invalid_output");
            result->error_message = g_strdup(error ? error->message : "写入回执缺少有效的同版控制状态");
        }
        g_clear_error(&error);
        y720_fan_status_clear(&status);
    }

    if (!result->ok) {
        if (g_strcmp0(result->error_code, "permission") == 0 && !self->elevating && !self->queued_mode) {
            gchar *binary = y720_cli_locate("y720-fan");
            if (binary && y720_cli_can_elevate(binary) && self->pending_args) {
                self->elevating = TRUE;
                self->busy = TRUE;
                banner_show(self, "需要管理员权限");
                update_controls(self);
                y720_cli_run_elevated(binary, (const gchar *const *)self->pending_args,
                                      self->pending_input, 60, self->cancellable, G_OBJECT(self), write_cb, self);
                g_free(binary);
                y720_result_free(result);
                return;
            }
            g_free(binary);
        }
        self->elevating = FALSE;
        const gchar *hint = y720_cli_error_hint(result->error_code);
        gchar *message = g_strdup(hint ? hint :
                                  (result->error_message ? result->error_message : "无法读取风扇状态"));
        banner_show(self, message);
        toast(self, "写入失败");
        g_free(message);
    } else {
        self->elevating = FALSE;
        if (self->pending_args && self->pending_args[1] && !strcmp(self->pending_args[1], "curve") &&
            self->pending_curve_generation == self->curve_generation)
            self->curve_dirty = FALSE;
        JsonObject *data = result->data;
        JsonNode *status_node = data ? json_object_get_member(data, "status") : NULL;
        if (status_node && JSON_NODE_HOLDS_OBJECT(status_node)) {
            apply_status_object(self, json_object_get_object_member(data, "status"));
        }
        if (self->curve_active && self->curve_enabled && self->curve_dirty &&
            self->pending_curve_generation != self->curve_generation && !self->curve_commit_id &&
            self->pending_args && self->pending_args[1] && !strcmp(self->pending_args[1], "curve"))
            self->queued_curve = 1;
        if (!self->curve_active || (!self->status.kernel_curve_error &&
            (!self->status.diagnostic_present[2] || !self->status.diagnostic[2])))
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
    if (self->queued_mode) {
        gchar *mode = g_steal_pointer(&self->queued_mode);
        y720_fan_page_set_mode(self, mode);
        g_free(mode);
        return;
    }
    if (self->queued_curve >= 0) {
        gboolean active = self->queued_curve != 0;
        self->queued_curve = -1;
        curve_set_active(self, active);
        return;
    }
    y720_fan_page_refresh(self);
}

static gboolean run_write_input(Y720FanPage *self, const gchar *const *args, const gchar *input, guint timeout)
{
    if (self->busy || !self->have_status || !self->status.controls_available ||
        g_strcmp0(self->status.driver_version, Y720_VERSION))
        return FALSE;
    gchar *binary = y720_cli_locate("y720-fan");
    if (!binary) {
        banner_show(self, "未找到 y720-fan；请安装或设置 Y720_FAN_BIN");
        return FALSE;
    }
    self->busy = TRUE;
    self->write_seq++;
    g_strfreev(self->pending_args);
    self->pending_args = g_strdupv((gchar **)args);
    g_free(self->pending_input);
    self->pending_input = g_strdup(input);
    update_controls(self);
    y720_cli_run(binary, args, input, timeout, self->cancellable, G_OBJECT(self), write_cb, self);
    g_free(binary);
    return TRUE;
}

static gboolean run_write(Y720FanPage *self, const gchar *const *args, guint timeout)
{
    return run_write_input(self, args, NULL, timeout);
}

void y720_fan_page_set_mode(Y720FanPage *self, const gchar *mode)
{
    g_return_if_fail(Y720_IS_FAN_PAGE(self));
    g_return_if_fail(mode != NULL);
    if (g_strcmp0(mode, "auto") && g_strcmp0(mode, "boost"))
        return;
    self->queued_curve = -1;
    g_clear_handle_id(&self->curve_commit_id, g_source_remove);
    if (self->busy) {
        g_free(self->queued_mode);
        self->queued_mode = g_strdup(mode);
        return;
    }
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
    return self->have_status && self->status.controls_available &&
           !g_strcmp0(self->status.driver_version, Y720_VERSION);
}

const gchar *y720_fan_page_mode(Y720FanPage *self)
{
    if (self->have_status && g_strcmp0(self->status.mode, "curve") == 0 && !self->curve_active)
        return "custom";
    return self->have_status ? self->status.mode : NULL;
}

gint y720_fan_page_rpm(Y720FanPage *self)
{
    return self->have_status ? (gint)self->status.fan_rpm[0] : -1;
}

static void on_mode_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)object;
    (void)pspec;
    Y720FanPage *self = Y720_FAN_PAGE(user_data);
    if (self->syncing)
        return;

    guint index = adw_toggle_group_get_active(ADW_TOGGLE_GROUP(self->mode_toggle));
    const gchar *mode = index == 1 ? "boost" :
                        index == 2 ? "custom" :
                        index == 3 ? "curve" : "auto";

    if (g_strcmp0(self->selected_mode, mode) == 0)
        return;
    g_free(self->selected_mode);
    self->selected_mode = g_strdup(mode);

    if (g_strcmp0(mode, "custom") == 0) {
        self->dirty = TRUE;
        if (self->curve_active)
            curve_set_active(self, FALSE);
        update_controls(self);
        return;
    }

    self->dirty = FALSE;
    if (g_strcmp0(mode, "curve") == 0) {
        if (self->curve_enabled)
            curve_set_active(self, TRUE);
    } else {
        y720_fan_page_set_mode(self, mode);
    }
    update_controls(self);
}

static void on_apply_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    Y720FanPage *self = Y720_FAN_PAGE(user_data);
    if (!self->have_status || !self->status.controls_available ||
        g_strcmp0(self->selected_mode, "custom") != 0)
        return;

    gint rpm = (gint)gtk_range_get_value(GTK_RANGE(self->rpm_scale));
    rpm = ((rpm + 99) / 100) * 100;
    gchar value[16];
    g_snprintf(value, sizeof(value), "%d", CLAMP(rpm, 1500, 5100));
    const gchar *args[] = {"--json", "set", "--rpm", value, NULL};
    run_write(self, args, 15);
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
    if (self->cancellable) {
        g_cancellable_cancel(self->cancellable);
        g_clear_object(&self->cancellable);
    }
    g_clear_pointer(&self->banner_text, g_free);
    g_clear_pointer(&self->queued_mode, g_free);
    y720_fan_status_clear(&self->status);
    g_clear_pointer(&self->selected_mode, g_free);
    g_clear_pointer(&self->pending_args, g_strfreev);
    g_clear_pointer(&self->pending_input, g_free);
    G_OBJECT_CLASS(y720_fan_page_parent_class)->dispose(object);
}


static void curve_set_active(Y720FanPage *self, gboolean active)
{
    if (!active)
        g_clear_handle_id(&self->curve_commit_id, g_source_remove);
    if (!active && !self->curve_active && !self->busy) return;
    if (!self->have_status || !self->status.kernel_curve_supported) {
        banner_show(self, "当前驱动不支持内核曲线，请安装支持 control_api=2 的驱动");
        return;
    }
    if (self->busy) { self->queued_curve = active ? 1 : 0; return; }
    if (!active && !self->curve_active) return;
    if (active) {
        for (guint i = 0; i < Y720_FAN_CURVE_POINTS; i++) {
            if (self->curve_points[i] % 100 ||
                (i && self->curve_points[i] < self->curve_points[i - 1])) {
                banner_show(self, "曲线转速必须按温度递增或保持不变，步长为 100 RPM");
                return;
            }
        }
    }
    self->pending_curve_generation = self->curve_generation;
    const gchar *args[] = {"--json", "curve", active ? "apply" : "stop", active ? "-" : NULL, NULL};
    gchar *input = active ? g_strdup_printf("%d %d %d %d %d %d\n", self->curve_points[0],
        self->curve_points[1], self->curve_points[2], self->curve_points[3], self->curve_points[4], self->curve_points[5]) : NULL;
    run_write_input(self, args, input, 15);
    g_free(input);
}

static void add_mode_toggle(Y720FanPage *self, const gchar *name, const gchar *label)
{
    AdwToggle *toggle = adw_toggle_new();
    adw_toggle_set_name(toggle, name);
    adw_toggle_set_label(toggle, label);
    adw_toggle_group_add(ADW_TOGGLE_GROUP(self->mode_toggle), toggle);
}

static void rebuild_mode_options(Y720FanPage *self)
{
    if (!self->mode_toggle)
        return;
    self->syncing = TRUE;
    adw_toggle_group_remove_all(ADW_TOGGLE_GROUP(self->mode_toggle));
    add_mode_toggle(self, "auto", y720_tr("自动"));
    add_mode_toggle(self, "boost", y720_tr("极致散热"));
    add_mode_toggle(self, "custom", y720_tr("手动"));
    if (self->curve_enabled)
        add_mode_toggle(self, "curve", y720_tr("曲线"));

    guint selected = g_strcmp0(self->selected_mode, "boost") == 0 ? 1 :
                     g_strcmp0(self->selected_mode, "custom") == 0 ? 2 :
                     g_strcmp0(self->selected_mode, "curve") == 0 ? 3 : 0;
    adw_toggle_group_set_active(ADW_TOGGLE_GROUP(self->mode_toggle), selected);
    self->syncing = FALSE;
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
    if (!enabled) {
        curve_set_active(self, FALSE);
        if (g_strcmp0(self->selected_mode, "curve") == 0) {
            g_free(self->selected_mode);
            self->selected_mode = g_strdup("custom");
        }
    }
    if (self->mode_toggle)
        rebuild_mode_options(self);
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

static gboolean commit_edited_curve(gpointer user_data)
{
    Y720FanPage *self = Y720_FAN_PAGE(user_data);
    self->curve_commit_id = 0;
    if (self->curve_active && self->curve_enabled)
        curve_set_active(self, TRUE);
    return G_SOURCE_REMOVE;
}

static void on_curve_point_changed(gpointer user_data)
{
    Y720FanPage *self = Y720_FAN_PAGE(user_data);
    if (self->curve_editor)
        y720_fan_curve_get_points(Y720_FAN_CURVE(self->curve_editor), self->curve_points);
    g_signal_emit(self, signals[SIGNAL_CURVE_CHANGED], 0);
    self->curve_dirty = TRUE;
    self->curve_generation++;
    if (self->curve_active) {
        g_clear_handle_id(&self->curve_commit_id, g_source_remove);
        self->curve_commit_id = g_timeout_add(350, commit_edited_curve, self);
    }
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
    self->queued_curve = -1;
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

    GtkWidget *hero = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 32);
    gtk_widget_set_margin_top(hero, 6);
    gtk_widget_set_margin_bottom(hero, 6);

    for (guint i = 0; i < 2; i++) {
        GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(column, TRUE);
        GtkWidget *caption = gtk_label_new(i == 0 ? "风扇 1" : "风扇 2");
        gtk_widget_add_css_class(caption, "dimmed");
        gtk_widget_set_halign(caption, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(column), caption);
        self->rpm_labels[i] = gtk_label_new("—");
        gtk_widget_add_css_class(self->rpm_labels[i], "rpm-value");
        gtk_widget_set_halign(self->rpm_labels[i], GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(column), self->rpm_labels[i]);
        gtk_box_append(GTK_BOX(hero), column);
    }

    self->mode_pill = gtk_label_new("等待状态");
    gtk_widget_add_css_class(self->mode_pill, "mode-pill");
    gtk_widget_set_valign(self->mode_pill, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(hero), self->mode_pill);
    gtk_box_append(GTK_BOX(content), hero);

    self->device_label = gtk_label_new("—");
    gtk_widget_add_css_class(self->device_label, "tiny");
    gtk_widget_add_css_class(self->device_label, "dimmed");
    gtk_widget_set_halign(self->device_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(content), self->device_label);

    GtkWidget *mode_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *mode_label = gtk_label_new("模式");
    gtk_widget_add_css_class(mode_label, "dimmed");
    gtk_widget_set_valign(mode_label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(mode_box), mode_label);
    self->mode_toggle = adw_toggle_group_new();
    adw_toggle_group_set_homogeneous(ADW_TOGGLE_GROUP(self->mode_toggle), TRUE);
    adw_toggle_group_set_can_shrink(ADW_TOGGLE_GROUP(self->mode_toggle), FALSE);
    gtk_widget_set_hexpand(self->mode_toggle, TRUE);
    g_signal_connect(self->mode_toggle, "notify::active", G_CALLBACK(on_mode_changed), self);
    gtk_box_append(GTK_BOX(mode_box), self->mode_toggle);
    gtk_box_append(GTK_BOX(content), mode_box);
    rebuild_mode_options(self);

    AdwPreferencesGroup *control_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(control_group, "手动转速");

    self->rpm_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1500, 5100, 100);
    gtk_widget_set_size_request(self->rpm_scale, 240, -1);
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
    self->custom_row = GTK_WIDGET(make_row("转速", scale_wrap));
    adw_preferences_group_add(control_group, self->custom_row);
    self->manual_group = GTK_WIDGET(control_group);
    gtk_box_append(GTK_BOX(content), self->manual_group);

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
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(curve_row), FALSE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(curve_row), FALSE);
    gtk_widget_set_focusable(curve_row, FALSE);
    gtk_widget_set_focusable(self->curve_editor, FALSE);
    adw_preferences_group_add(curve_group, curve_row);

    self->curve_temp_label = make_value_label("—");
    adw_preferences_group_add(curve_group, GTK_WIDGET(make_row("温度", self->curve_temp_label)));

    gtk_widget_set_visible(self->curve_group, FALSE);
    gtk_box_append(GTK_BOX(content), self->curve_group);

    self->refresh_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(self->refresh_button, "刷新状态");
    g_signal_connect(self->refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), self);
    self->apply_button = gtk_button_new_with_label("应用");
    gtk_widget_add_css_class(self->apply_button, "suggested-action");
    g_signal_connect(self->apply_button, "clicked", G_CALLBACK(on_apply_clicked), self);

    GtkWidget *action_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(action_bar, "action-bar");
    gtk_widget_set_halign(action_bar, GTK_ALIGN_END);
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
    self->poll_id = g_timeout_add_seconds(5, poll_cb, self);
}

void y720_fan_page_stop(Y720FanPage *self)
{
    g_return_if_fail(Y720_IS_FAN_PAGE(self));
    g_clear_handle_id(&self->curve_commit_id, g_source_remove);
    self->started = FALSE;
    if (self->poll_id) {
        g_source_remove(self->poll_id);
        self->poll_id = 0;
    }
}
