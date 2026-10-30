/* SPDX-License-Identifier: MIT */
#include "y720-models.h"
#include "y720-i18n.h"

#include <string.h>

const gchar *const y720_fan_diagnostic_names[Y720_FAN_DIAGNOSTIC_COUNT] = {
    "requested_mode", "safety_fault", "last_error",
    "target_writes", "mode_writes", "ramp_low_samples",
};

static gboolean parse_error(GError **error, const gchar *message)
{
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, y720_tr("%s"), y720_tr(message));
    return FALSE;
}

static gboolean read_bool(JsonObject *object, const gchar *name, gboolean *value, GError **error)
{
    if (!json_object_has_member(object, name) ||
        !JSON_NODE_HOLDS_VALUE(json_object_get_member(object, name)) ||
        json_node_get_value_type(json_object_get_member(object, name)) != G_TYPE_BOOLEAN) {
        gchar *message = g_strdup_printf(y720_tr("字段 %s 缺失或不是布尔值"), name);
        parse_error(error, message);
        g_free(message);
        return FALSE;
    }
    *value = json_object_get_boolean_member(object, name);
    return TRUE;
}

static gboolean read_string(JsonObject *object, const gchar *name, gchar **value,
                            gboolean nullable, GError **error)
{
    *value = NULL;
    if (!json_object_has_member(object, name)) {
        if (nullable)
            return TRUE;
        gchar *message = g_strdup_printf(y720_tr("字段 %s 缺失"), name);
        parse_error(error, message);
        g_free(message);
        return FALSE;
    }
    JsonNode *node = json_object_get_member(object, name);
    if (JSON_NODE_HOLDS_NULL(node)) {
        if (nullable)
            return TRUE;
        gchar *message = g_strdup_printf(y720_tr("字段 %s 不能为 null"), name);
        parse_error(error, message);
        g_free(message);
        return FALSE;
    }
    if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING) {
        gchar *message = g_strdup_printf(y720_tr("字段 %s 不是字符串"), name);
        parse_error(error, message);
        g_free(message);
        return FALSE;
    }
    *value = g_strdup(json_object_get_string_member(object, name));
    return TRUE;
}

static gboolean read_int(JsonObject *object, const gchar *name, gint64 *value,
                         gboolean nullable, GError **error)
{
    *value = -1;
    if (!json_object_has_member(object, name)) {
        if (nullable)
            return TRUE;
        gchar *message = g_strdup_printf(y720_tr("字段 %s 缺失"), name);
        parse_error(error, message);
        g_free(message);
        return FALSE;
    }
    JsonNode *node = json_object_get_member(object, name);
    if (JSON_NODE_HOLDS_NULL(node)) {
        if (nullable)
            return TRUE;
        gchar *message = g_strdup_printf(y720_tr("字段 %s 不能为 null"), name);
        parse_error(error, message);
        g_free(message);
        return FALSE;
    }
    if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64) {
        gchar *message = g_strdup_printf(y720_tr("字段 %s 不是整数"), name);
        parse_error(error, message);
        g_free(message);
        return FALSE;
    }
    *value = json_object_get_int_member(object, name);
    return TRUE;
}

static gboolean read_int_array4(JsonObject *object, const gchar *name, gint values[4], GError **error)
{
    if (!json_object_has_member(object, name) ||
        !JSON_NODE_HOLDS_ARRAY(json_object_get_member(object, name))) {
        gchar *message = g_strdup_printf(y720_tr("字段 %s 不是数组"), name);
        parse_error(error, message);
        g_free(message);
        return FALSE;
    }
    JsonArray *array = json_object_get_array_member(object, name);
    if (json_array_get_length(array) != 4) {
        gchar *message = g_strdup_printf(y720_tr("字段 %s 需要 4 个元素"), name);
        parse_error(error, message);
        g_free(message);
        return FALSE;
    }
    for (guint i = 0; i < 4; i++) {
        JsonNode *node = json_array_get_element(array, i);
        if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64) {
            gchar *message = g_strdup_printf(y720_tr("字段 %s[%u] 不是整数"), name, i);
            parse_error(error, message);
            g_free(message);
            return FALSE;
        }
        gint64 value = json_node_get_int(node);
        gint64 upper = g_str_equal(name, "colors") ? 19 : 3;
        if (value < 0 || value > upper)
            return parse_error(error, "四区配置数值超出范围");
        values[i] = (gint)value;
    }
    return TRUE;
}

void y720_fan_status_clear(Y720FanStatus *status)
{
    if (!status)
        return;
    g_free(status->device);
    g_free(status->driver_version);
    g_free(status->mode);
    g_free(status->temperature_source);
    memset(status, 0, sizeof(*status));
}

gboolean y720_fan_status_parse(JsonObject *data, Y720FanStatus *status, GError **error)
{
    Y720FanStatus parsed = {0};
    if (!data)
        return parse_error(error, "风扇 status 缺少 data 对象");

    if (!read_bool(data, "controls_available", &parsed.controls_available, error) ||
        !read_string(data, "device", &parsed.device, FALSE, error) ||
        !read_string(data, "driver_version", &parsed.driver_version, FALSE, error) ||
        !read_string(data, "mode", &parsed.mode, TRUE, error))
        goto fail;

    gint64 value = 0;
    if (!read_int(data, "mode_raw", &value, TRUE, error))
        goto fail;
    parsed.mode_raw = value;
    if (!read_int(data, "target_rpm", &value, TRUE, error))
        goto fail;
    parsed.target_rpm = value;
    if (!read_int(data, "pwm", &value, TRUE, error))
        goto fail;
    parsed.pwm = value;

    if (!json_object_has_member(data, "fan_rpm") ||
        !JSON_NODE_HOLDS_ARRAY(json_object_get_member(data, "fan_rpm"))) {
        parse_error(error, "字段 fan_rpm 不是数组");
        goto fail;
    }
    JsonArray *fans = json_object_get_array_member(data, "fan_rpm");
    if (json_array_get_length(fans) != 2) {
        parse_error(error, "字段 fan_rpm 需要 2 个元素");
        goto fail;
    }
    for (guint i = 0; i < 2; i++) {
        JsonNode *node = json_array_get_element(fans, i);
        if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64) {
            parse_error(error, "字段 fan_rpm 包含非整数");
            goto fail;
        }
        parsed.fan_rpm[i] = json_node_get_int(node);
    }

    if (!json_object_has_member(data, "diagnostics") ||
        !JSON_NODE_HOLDS_OBJECT(json_object_get_member(data, "diagnostics"))) {
        parse_error(error, "字段 diagnostics 不是对象");
        goto fail;
    }
    JsonObject *diagnostics = json_object_get_object_member(data, "diagnostics");
    for (guint i = 0; i < Y720_FAN_DIAGNOSTIC_COUNT; i++) {
        const gchar *name = y720_fan_diagnostic_names[i];
        if (!json_object_has_member(diagnostics, name))
            continue;
        JsonNode *node = json_object_get_member(diagnostics, name);
        if (JSON_NODE_HOLDS_NULL(node))
            continue;
        if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64) {
            gchar *message = g_strdup_printf(y720_tr("诊断字段 %s 不是整数"), name);
            parse_error(error, message);
            g_free(message);
            goto fail;
        }
        parsed.diagnostic_present[i] = TRUE;
        parsed.diagnostic[i] = json_node_get_int(node);
    }

    if (parsed.controls_available &&
        (parsed.mode_raw < 0 || parsed.mode_raw > 3 || parsed.target_rpm < 1500 ||
         parsed.target_rpm > 5100 || parsed.pwm < 0 || parsed.pwm > 255 ||
         g_strcmp0(parsed.mode, parsed.mode_raw == 0 ? "boost" : parsed.mode_raw == 2 ? "auto" : "curve"))) {
        parse_error(error, "风扇控制状态超出范围或模式不一致");
        goto fail;
    }
    for (guint i = 0; i < 2; i++) {
        if (parsed.fan_rpm[i] < 0 || parsed.fan_rpm[i] > 20000) {
            parse_error(error, "实际转速超出有效范围");
            goto fail;
        }
    }
    if (json_object_has_member(data, "temperature")) {
        JsonNode *node = json_object_get_member(data, "temperature");
        if (!JSON_NODE_HOLDS_OBJECT(node)) {
            parse_error(error, "temperature 不是对象");
            goto fail;
        }
        JsonObject *temperature = json_node_get_object(node);
        if (!read_int(temperature, "millidegrees", &parsed.temperature_mc, TRUE, error) ||
            !read_string(temperature, "source", &parsed.temperature_source, TRUE, error)) goto fail;
        if (parsed.temperature_source && strcmp(parsed.temperature_source, "x86_pkg_temp") &&
            strcmp(parsed.temperature_source, "coretemp")) {
            parse_error(error, "控制温度来源无效");
            goto fail;
        }
        parsed.temperature_valid = parsed.temperature_mc >= 1000 && parsed.temperature_mc <= 120000 &&
                                   parsed.temperature_source != NULL;
    }
    parsed.kernel_curve_temp = -1;
    if (json_object_has_member(data, "kernel_curve")) {
        JsonNode *node = json_object_get_member(data, "kernel_curve");
        if (!JSON_NODE_HOLDS_OBJECT(node)) { parse_error(error, "内核曲线状态无效"); goto fail; }
        JsonObject *curve = json_node_get_object(node);
        gchar *engine = NULL;
        if (!read_string(curve, "engine", &engine, TRUE, error)) goto fail;
        gboolean backend_valid = !g_strcmp0(engine, "linux-thermal");
        g_free(engine);
        if (!read_bool(curve, "supported", &parsed.kernel_curve_supported, error) ||
            !read_bool(curve, "active", &parsed.kernel_curve_active, error) ||
            !read_int(curve, "error", &parsed.kernel_curve_error, FALSE, error) ||
            !read_int(curve, "temperature_millidegrees", &parsed.kernel_curve_temp, TRUE, error)) goto fail;
        if ((parsed.kernel_curve_supported && !backend_valid) ||
            (parsed.kernel_curve_active && !parsed.kernel_curve_supported)) {
            parse_error(error, "内核曲线后端或控制状态不一致"); goto fail;
        }
        JsonNode *points = json_object_get_member(curve, "points");
        if (!points || !JSON_NODE_HOLDS_ARRAY(points) || json_array_get_length(json_node_get_array(points)) != 6) {
            parse_error(error, "内核曲线需要六个转速点"); goto fail;
        }
        for (guint i = 0; i < 6; i++) {
            JsonNode *point = json_array_get_element(json_node_get_array(points), i);
            if (!JSON_NODE_HOLDS_VALUE(point) || json_node_get_value_type(point) != G_TYPE_INT64) {
                parse_error(error, "内核曲线转速无效"); goto fail;
            }
            gint64 rpm = json_node_get_int(point);
            if (rpm < G_MININT || rpm > G_MAXINT ||
                (parsed.kernel_curve_supported && (rpm < 1500 || rpm > 5100 || rpm % 100 ||
                 (i && rpm < parsed.kernel_curve_points[i - 1])))) {
                parse_error(error, "内核曲线转速超出范围"); goto fail;
            }
            parsed.kernel_curve_points[i] = (gint)rpm;
        }
    }
    y720_fan_status_clear(status);
    *status = parsed;
    return TRUE;

fail:
    y720_fan_status_clear(&parsed);
    return FALSE;
}

void y720_backlight_profile_clear(Y720BacklightProfile *profile)
{
    if (!profile)
        return;
    g_free(profile->effect);
    memset(profile, 0, sizeof(*profile));
}

void y720_backlight_profile_copy(Y720BacklightProfile *dst, const Y720BacklightProfile *src)
{
    y720_backlight_profile_clear(dst);
    dst->effect = g_strdup(src->effect);
    memcpy(dst->colors, src->colors, sizeof(dst->colors));
    memcpy(dst->brightness, src->brightness, sizeof(dst->brightness));
}

static gboolean parse_profile_object(JsonObject *object, Y720BacklightProfile *profile, GError **error)
{
    Y720BacklightProfile parsed = {0};
    if (!object)
        return parse_error(error, "背光 profile 不是对象");
    if (!read_string(object, "effect", &parsed.effect, FALSE, error) ||
        !read_int_array4(object, "colors", parsed.colors, error) ||
        !read_int_array4(object, "brightness", parsed.brightness, error))
        goto fail;
    gboolean known_effect = FALSE;
    for (gint i = 0; y720_effect_name(i); i++)
        known_effect |= g_strcmp0(parsed.effect, y720_effect_name(i)) == 0;
    if (!known_effect) {
        parse_error(error, "背光效果无效");
        goto fail;
    }
    y720_backlight_profile_clear(profile);
    *profile = parsed;
    return TRUE;
fail:
    y720_backlight_profile_clear(&parsed);
    return FALSE;
}

void y720_backlight_info_clear(Y720BacklightInfo *info)
{
    if (!info)
        return;
    g_free(info->device);
    g_free(info->hid_device);
    g_free(info->driver);
    g_free(info->descriptor_sha256);
    y720_backlight_profile_clear(&info->profile);
    memset(info, 0, sizeof(*info));
}

gboolean y720_backlight_info_parse(JsonObject *data, Y720BacklightInfo *info, GError **error)
{
    Y720BacklightInfo parsed = {0};
    if (!data)
        return parse_error(error, "背光 status 缺少 data 对象");

    if (!read_string(data, "device", &parsed.device, FALSE, error) ||
        !read_string(data, "hid_device", &parsed.hid_device, FALSE, error) ||
        !read_string(data, "driver", &parsed.driver, FALSE, error) ||
        !read_string(data, "descriptor_sha256", &parsed.descriptor_sha256, FALSE, error))
        goto fail;

    gint64 report_bytes = 0;
    if (!read_int(data, "report_bytes", &report_bytes, FALSE, error))
        goto fail;
    if (report_bytes != 7) {
        parse_error(error, "背光报告长度不受支持");
        goto fail;
    }
    parsed.report_bytes = (gint)report_bytes;

    if (!read_bool(data, "state_known", &parsed.state_known, error))
        goto fail;

    if (!json_object_has_member(data, "profile")) {
        parse_error(error, "字段 profile 缺失");
        goto fail;
    }
    JsonNode *profile_node = json_object_get_member(data, "profile");
    if (JSON_NODE_HOLDS_OBJECT(profile_node)) {
        if (!parse_profile_object(json_object_get_object_member(data, "profile"),
                                  &parsed.profile, error))
            goto fail;
        parsed.has_profile = TRUE;
    } else if (!JSON_NODE_HOLDS_NULL(profile_node)) {
        parse_error(error, "字段 profile 既不是对象也不是 null");
        goto fail;
    }

    y720_backlight_info_clear(info);
    *info = parsed;
    return TRUE;
fail:
    y720_backlight_info_clear(&parsed);
    return FALSE;
}

const gchar *y720_color_name(gint index)
{
    static const gchar *const names[20] = {
        "red", "torch-red", "pink", "magenta", "purple",
        NULL, "blue", NULL, NULL, "cyan",
        NULL, NULL, "green", NULL, NULL,
        "yellow", "orange", NULL, "white", "none",
    };
    if (index < 0 || index >= 20)
        return NULL;
    return names[index];
}

const gchar *y720_color_label(gint index)
{
    static const gchar *const labels[20] = {
        "红", "火炬红", "粉", "洋红", "紫",
        "灰", "蓝", "深蓝", "浅蓝", "青",
        "深绿", "亮绿", "绿", "淡绿", "橄榄",
        "黄", "橙", "深橙", "白", "无",
    };
    if (index < 0 || index >= 20)
        return NULL;
    return labels[index];
}

const gchar *y720_effect_name(gint index)
{
    static const gchar *const names[] = {"heartbeat", "breath", "smooth", "static", "wave"};
    if (index < 0 || index >= (gint)G_N_ELEMENTS(names))
        return NULL;
    return names[index];
}

const gchar *y720_effect_label(gint index)
{
    static const gchar *const labels[] = {"心跳", "呼吸", "平滑", "常亮", "波浪"};
    if (index < 0 || index >= (gint)G_N_ELEMENTS(labels))
        return NULL;
    return labels[index];
}

gboolean y720_color_from_name(const gchar *name, gint *index)
{
    if (!name)
        return FALSE;
    for (gint i = 0; i < 20; i++) {
        if (g_strcmp0(y720_color_name(i), name) == 0) {
            *index = i;
            return TRUE;
        }
    }
    gchar *end = NULL;
    gint64 parsed = g_ascii_strtoll(name, &end, 10);
    if (end && *end == '\0' && parsed >= 0 && parsed <= 19) {
        *index = (gint)parsed;
        return TRUE;
    }
    return FALSE;
}

gint y720_effect_index(const gchar *name)
{
    for (gint i = 0; i < 5; i++)
        if (g_strcmp0(y720_effect_name(i), name) == 0)
            return i;
    return 3;
}

gchar *y720_backlight_profile_to_json(const Y720BacklightProfile *profile)
{
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "schema_version");
    json_builder_add_int_value(builder, 1);
    json_builder_set_member_name(builder, "effect");
    json_builder_add_string_value(builder, profile->effect ? profile->effect : "static");

    json_builder_set_member_name(builder, "colors");
    json_builder_begin_array(builder);
    for (guint i = 0; i < 4; i++) {
        const gchar *name = y720_color_name(profile->colors[i]);
        if (name) {
            json_builder_add_string_value(builder, name);
        } else {
            gchar number[8];
            g_snprintf(number, sizeof(number), "%d", profile->colors[i]);
            json_builder_add_string_value(builder, number);
        }
    }
    json_builder_end_array(builder);

    json_builder_set_member_name(builder, "brightness");
    json_builder_begin_array(builder);
    for (guint i = 0; i < 4; i++)
        json_builder_add_int_value(builder, profile->brightness[i]);
    json_builder_end_array(builder);
    json_builder_end_object(builder);

    JsonGenerator *generator = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    gchar *text = json_generator_to_data(generator, NULL);
    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);
    return text;
}
