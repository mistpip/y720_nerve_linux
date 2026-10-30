/* SPDX-License-Identifier: MIT */
#include "y720-cli.h"
#include "y720-i18n.h"

#include <signal.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

typedef struct {
    GSubprocess *process;
    GCancellable *cancellable;
    GSource *cancel_source;
    GWeakRef owner;
    gboolean has_owner;
    guint timeout_id;
    guint force_id;
    gboolean timed_out;
    gboolean cancelled;
    gboolean finished;
    gchar *expected_program;
    gchar *input;
    Y720CliCallback callback;
    gpointer user_data;
} CliCall;

/* All calls and this registry are confined to the GTK main context. */
static GList *active_calls;

static void cli_call_free(CliCall *call)
{
    active_calls = g_list_remove(active_calls, call);
    if (call->timeout_id)
        g_source_remove(call->timeout_id);
    if (call->force_id)
        g_source_remove(call->force_id);
    if (call->cancel_source) {
        g_source_destroy(call->cancel_source);
        g_source_unref(call->cancel_source);
    }
    g_weak_ref_clear(&call->owner);
    g_clear_object(&call->process);
    g_clear_object(&call->cancellable);
    g_free(call->expected_program);
    g_free(call->input);
    g_free(call);
}

gchar *y720_cli_locate(const gchar *name)
{
    const gchar *env = NULL;
    if (g_strcmp0(name, "y720-fan") == 0)
        env = g_getenv("Y720_FAN_BIN");
    else if (g_strcmp0(name, "y720-backlight") == 0)
        env = g_getenv("Y720_BACKLIGHT_BIN");

    if (env && *env) {
        gchar *path = g_canonicalize_filename(env, NULL);
        if (g_file_test(path, G_FILE_TEST_IS_EXECUTABLE))
            return path;
        g_free(path);
    }

    gchar *executable = g_file_read_link("/proc/self/exe", NULL);
    if (executable) {
        gchar *dir = g_path_get_dirname(executable);
        gchar *candidate = g_build_filename(dir, name, NULL);
        g_free(dir);
        g_free(executable);
        if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE))
            return candidate;
        g_free(candidate);
    }
    return g_find_program_in_path(name);
}

void y720_result_free(Y720Result *result)
{
    if (!result)
        return;
    g_free(result->program);
    g_free(result->version);
    g_free(result->stdout_text);
    g_free(result->stderr_text);
    g_clear_pointer(&result->data, json_object_unref);
    g_free(result->error_code);
    g_free(result->error_message);
    g_clear_pointer(&result->error_details, json_object_unref);
    g_free(result->transport_error);
    g_free(result->expected_program);
    g_free(result);
}

static Y720Result *result_new(void)
{
    Y720Result *result = g_new0(Y720Result, 1);
    result->exit_code = -1;
    return result;
}

static void result_set_local_error(Y720Result *result, const gchar *code, const gchar *message)
{
    g_free(result->transport_error);
    result->transport_error = g_strdup(code);
    result->ok = FALSE;
    if (!result->error_code)
        result->error_code = g_strdup(code);
    if (!result->error_message)
        result->error_message = g_strdup(y720_tr(message));
}

static gboolean member_is(JsonObject *object, const gchar *name, GType type)
{
    if (!json_object_has_member(object, name))
        return FALSE;
    JsonNode *node = json_object_get_member(object, name);
    return node && json_node_get_value_type(node) == type;
}

static void parse_cli_json(Y720Result *result)
{
    const gchar *source = NULL;
    if (result->exit_code == 0 && result->stdout_text && *result->stdout_text)
        source = result->stdout_text;
    else if (result->stderr_text && *result->stderr_text)
        source = result->stderr_text;
    else if (result->stdout_text && *result->stdout_text)
        source = result->stdout_text;

    if (!source) {
        if (result->cancelled)
            result_set_local_error(result, "cancelled", "操作已取消");
        else if (result->timed_out)
            result_set_local_error(result, "timeout", "CLI 在规定时间内没有完成");
        else
            result_set_local_error(result, "invalid_output", "CLI 没有返回任何 JSON 输出");
        return;
    }

    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    if (!json_parser_load_from_data(parser, source, -1, &error)) {
        gchar *detail = g_strdup_printf(y720_tr("CLI 输出不是合法 JSON：%s"), error->message);
        result_set_local_error(result, "invalid_output", detail);
        g_free(detail);
        g_error_free(error);
        g_object_unref(parser);
        return;
    }

    JsonNode *root_node = json_parser_get_root(parser);
    if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node)) {
        result_set_local_error(result, "invalid_output", "CLI 输出 JSON 不是对象");
        g_object_unref(parser);
        return;
    }

    JsonObject *root = json_node_get_object(root_node);
    if (!member_is(root, "schema_version", G_TYPE_INT64) ||
        json_object_get_int_member(root, "schema_version") != 1) {
        result_set_local_error(result, "unsupported_schema", "CLI 输出的 schema_version 不是 1");
        g_object_unref(parser);
        return;
    }

    if (!member_is(root, "program", G_TYPE_STRING) || !member_is(root, "version", G_TYPE_STRING)) {
        result_set_local_error(result, "invalid_output", "CLI 输出缺少 program 或 version");
        g_object_unref(parser);
        return;
    }
    if (member_is(root, "program", G_TYPE_STRING)) {
        result->program = g_strdup(json_object_get_string_member(root, "program"));
        if (result->expected_program && g_strcmp0(result->program, result->expected_program) != 0) {
            gchar *detail = g_strdup_printf(y720_tr("期望 %s，实际 %s"), result->expected_program, result->program);
            result_set_local_error(result, "invalid_output", detail);
            g_free(detail);
            g_object_unref(parser);
            return;
        }
    }

    if (member_is(root, "version", G_TYPE_STRING))
        result->version = g_strdup(json_object_get_string_member(root, "version"));
    if (g_strcmp0(result->version, Y720_VERSION) != 0) {
        result_set_local_error(result, "unsupported_version", "CLI 与 GUI 版本不一致，请安装同版组件");
        g_object_unref(parser);
        return;
    }

    if (!member_is(root, "ok", G_TYPE_BOOLEAN)) {
        result_set_local_error(result, "invalid_output", "CLI 输出缺少布尔字段 ok");
        g_object_unref(parser);
        return;
    }

    gboolean json_ok = json_object_get_boolean_member(root, "ok");

    if (result->exit_code == 0) {
        if (!json_ok) {
            result_set_local_error(result, "invalid_output", "CLI 退出码为 0 但 ok=false");
            g_object_unref(parser);
            return;
        }
        if (!member_is(root, "data", JSON_TYPE_OBJECT)) {
            result_set_local_error(result, "invalid_output", "CLI 成功输出缺少 data 对象");
            g_object_unref(parser);
            return;
        }
        result->data = json_object_ref(json_object_get_object_member(root, "data"));
        result->ok = TRUE;
    } else {
        if (json_ok) {
            result_set_local_error(result, "invalid_output", "CLI 失败退出但 ok=true");
            g_object_unref(parser);
            return;
        }
        if (member_is(root, "error", JSON_TYPE_OBJECT)) {
            JsonObject *error_object = json_object_get_object_member(root, "error");
            if (member_is(error_object, "code", G_TYPE_STRING))
                result->error_code = g_strdup(json_object_get_string_member(error_object, "code"));
            if (member_is(error_object, "message", G_TYPE_STRING))
                result->error_message = g_strdup(json_object_get_string_member(error_object, "message"));
            if (member_is(error_object, "details", JSON_TYPE_OBJECT))
                result->error_details = json_object_ref(json_object_get_object_member(error_object, "details"));
        }
        if (!result->error_code)
            result->error_code = g_strdup(result->cancelled ? "cancelled" :
                                          result->timed_out ? "timeout" : "cli_error");
        if (!result->error_message) {
            const gchar *raw = result->stderr_text && *result->stderr_text
                                   ? result->stderr_text : "CLI 失败但没有错误信息";
            result->error_message = g_strdup(raw);
        }
        result->ok = FALSE;
    }

    g_object_unref(parser);
}

static void finish_call(CliCall *call, Y720Result *result)
{
    if (call->finished) {
        y720_result_free(result);
        return;
    }
    call->finished = TRUE;

    if (result->timed_out)
        result_set_local_error(result, "timeout", "CLI 在规定时间内没有完成，已请求终止");
    if (result->cancelled)
        result_set_local_error(result, "cancelled", "操作已取消；硬件状态可能已经改变");
    if (!result->ok && result->term_sig && !result->stderr_text)
        result_set_local_error(result, "cancelled", "CLI 被信号终止");

    GObject *owner = g_weak_ref_get(&call->owner);
    if (call->callback && (!call->has_owner || owner))
        call->callback(result, call->user_data);
    else
        y720_result_free(result);
    g_clear_object(&owner);

    cli_call_free(call);
}

static gboolean on_force_exit(gpointer user_data)
{
    CliCall *call = user_data;
    call->force_id = 0;
    if (call->process)
        g_subprocess_force_exit(call->process);
    return G_SOURCE_REMOVE;
}

static void terminate(CliCall *call)
{
    if (!call->process)
        return;
    g_subprocess_send_signal(call->process, SIGTERM);
    if (!call->force_id)
        call->force_id = g_timeout_add(1200, on_force_exit, call);
}

void y720_cli_cancel_all(void)
{
    for (GList *item = active_calls; item; item = item->next) {
        CliCall *call = item->data;
        call->cancelled = TRUE;
        terminate(call);
    }
}

gboolean y720_cli_has_pending(void)
{
    return active_calls != NULL;
}

static gboolean on_timeout(gpointer user_data)
{
    CliCall *call = user_data;
    call->timeout_id = 0;
    call->timed_out = TRUE;
    terminate(call);
    return G_SOURCE_REMOVE;
}

static gboolean on_cancelled(GCancellable *cancellable, gpointer user_data)
{
    (void)cancellable;
    CliCall *call = user_data;
    call->cancelled = TRUE;
    terminate(call);
    return G_SOURCE_REMOVE;
}

static void on_communicate(GObject *source, GAsyncResult *res, gpointer user_data)
{
    CliCall *call = user_data;
    GError *error = NULL;
    gchar *stdout_text = NULL;
    gchar *stderr_text = NULL;

    gboolean done = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), res,
                                                         &stdout_text, &stderr_text, &error);

    if (call->timeout_id) {
        g_source_remove(call->timeout_id);
        call->timeout_id = 0;
    }

    Y720Result *result = result_new();
    result->timed_out = call->timed_out;
    result->cancelled = call->cancelled;
    result->stdout_text = stdout_text;
    result->stderr_text = stderr_text;
    result->expected_program = g_strdup(call->expected_program);

    if (!done) {
        result_set_local_error(result, "io_error", error ? error->message : "与 CLI 通信失败");
        g_clear_error(&error);
    } else {
        if (g_subprocess_get_if_signaled(call->process))
            result->term_sig = g_subprocess_get_term_sig(call->process);
        if (g_subprocess_get_if_exited(call->process))
            result->exit_code = g_subprocess_get_exit_status(call->process);
        if (result->timed_out || result->cancelled)
            result->exit_code = result->exit_code == 0 ? 143 : result->exit_code;
        parse_cli_json(result);
    }

    finish_call(call, result);
}

void y720_cli_run(const gchar *binary,
                  const gchar *const *args,
                  const gchar *input,
                  guint timeout_seconds,
                  GCancellable *cancellable,
                  GObject *owner,
                  Y720CliCallback callback,
                  gpointer user_data)
{
    CliCall *call = g_new0(CliCall, 1);
    g_weak_ref_init(&call->owner, owner);
    call->has_owner = owner != NULL;
    call->input = g_strdup(input);
    call->callback = callback;
    call->user_data = user_data;
    call->cancellable = cancellable ? g_object_ref(cancellable) : NULL;

    const gchar *target = binary;
    if (g_str_has_suffix(binary, "pkexec") && args && args[0]) {
        target = args[0];
    } else if (g_str_has_suffix(binary, "sudo") && args) {
        for (const gchar *const *arg = args; arg && *arg; arg++) {
            if (strstr(*arg, "y720-fan") || strstr(*arg, "y720-backlight")) {
                target = *arg;
                break;
            }
        }
    }
    if (strstr(target, "y720-fan"))
        call->expected_program = g_strdup("y720-fan");
    else if (strstr(target, "y720-backlight"))
        call->expected_program = g_strdup("y720-backlight");

    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, (gpointer)binary);
    for (const gchar *const *arg = args; arg && *arg; arg++)
        g_ptr_array_add(argv, (gpointer)*arg);
    g_ptr_array_add(argv, NULL);

    GError *error = NULL;
    call->process = g_subprocess_newv((const gchar *const *)argv->pdata,
                                      G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                      G_SUBPROCESS_FLAGS_STDERR_PIPE |
                                      G_SUBPROCESS_FLAGS_STDIN_PIPE,
                                      &error);
    g_ptr_array_free(argv, TRUE);

    if (!call->process) {
        Y720Result *result = result_new();
        result_set_local_error(result, "spawn_failed", error ? error->message : "无法启动 CLI");
        g_clear_error(&error);
        finish_call(call, result);
        return;
    }

    if (cancellable) {
        /* Dispatch cancellation on the same main context as completion. */
        call->cancel_source = g_cancellable_source_new(cancellable);
        g_source_set_callback(call->cancel_source, G_SOURCE_FUNC(on_cancelled), call, NULL);
        g_source_attach(call->cancel_source, NULL);
    }

    if (timeout_seconds > 0)
        call->timeout_id = g_timeout_add_seconds(timeout_seconds, on_timeout, call);

    active_calls = g_list_prepend(active_calls, call);
    g_subprocess_communicate_utf8_async(call->process, call->input, NULL, on_communicate, call);
}

const gchar *y720_cli_error_hint(const gchar *code)
{
    if (!code)
        return NULL;
    if (g_strcmp0(code, "permission") == 0)
        return y720_tr("需要管理员权限。");
    if (g_strcmp0(code, "read_only") == 0)
        return y720_tr("驱动以只读方式加载，未开放控制接口。");
    if (g_strcmp0(code, "device_not_found") == 0 || g_strcmp0(code, "unsupported_device") == 0)
        return y720_tr("没有检测到 Y720 硬件。");
    if (g_strcmp0(code, "busy") == 0)
        return y720_tr("另一个控制程序占用了设备。");
    if (g_strcmp0(code, "write_uncertain") == 0)
        return y720_tr("写入可能只完成了一部分，请重新读取状态。");
    if (g_strcmp0(code, "timeout") == 0)
        return y720_tr("等待转速收敛超时，目标仍然保留。");
    if (g_strcmp0(code, "superseded") == 0)
        return y720_tr("模式或目标已被其他程序改写。");
    if (g_strcmp0(code, "unsupported_version") == 0)
        return y720_tr("驱动版本不支持写入。");
    if (g_strcmp0(code, "io_error") == 0)
        return y720_tr("读写驱动接口失败。");
    if (g_strcmp0(code, "invalid_output") == 0 || g_strcmp0(code, "unsupported_schema") == 0)
        return y720_tr("CLI 输出格式不符合预期。");
    if (g_strcmp0(code, "spawn_failed") == 0)
        return y720_tr("无法启动 CLI。");
    if (g_strcmp0(code, "cancelled") == 0)
        return y720_tr("操作已取消。");
    return NULL;
}

gboolean y720_cli_can_elevate(const gchar *binary)
{
    if (!binary || !g_path_is_absolute(binary))
        return FALSE;
    const gchar *fan_override = g_getenv("Y720_FAN_BIN");
    const gchar *light_override = g_getenv("Y720_BACKLIGHT_BIN");
    if ((fan_override && *fan_override) || (light_override && *light_override))
        return FALSE;
    gchar *name = g_path_get_basename(binary);
    gboolean known = g_str_equal(name, "y720-fan") || g_str_equal(name, "y720-backlight");
    g_free(name);
    if (!known || !g_str_has_prefix(binary, Y720_BINDIR "/"))
        return FALSE;
    struct stat info;
    if (lstat(binary, &info) != 0 || !S_ISREG(info.st_mode))
        return FALSE;
    if (info.st_uid != 0 || (info.st_mode & (S_IWGRP | S_IWOTH)))
        return FALSE;
    gchar *dir = g_path_get_dirname(binary);
    gboolean trusted = TRUE;
    for (;;) {
        if (lstat(dir, &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != 0 ||
            (info.st_mode & (S_IWGRP | S_IWOTH))) {
            trusted = FALSE;
            break;
        }
        if (g_str_equal(dir, "/"))
            break;
        gchar *parent = g_path_get_dirname(dir);
        g_free(dir);
        dir = parent;
    }
    g_free(dir);
    return trusted && g_file_test("/usr/bin/sudo", G_FILE_TEST_IS_EXECUTABLE);
}

void y720_cli_run_elevated(const gchar *binary,
                           const gchar *const *args,
                           const gchar *input,
                           guint timeout_seconds,
                           GCancellable *cancellable,
                           GObject *owner,
                           Y720CliCallback callback,
                           gpointer user_data)
{
    if (!y720_cli_can_elevate(binary)) {
        Y720Result *result = result_new();
        result_set_local_error(result, "permission", "拒绝提权运行未核验的 CLI 路径");
        callback(result, user_data);
        return;
    }
    GPtrArray *full = g_ptr_array_new();
    g_ptr_array_add(full, (gpointer)"-n");
    g_ptr_array_add(full, (gpointer)binary);
    for (const gchar *const *arg = args; arg && *arg; arg++)
        g_ptr_array_add(full, (gpointer)*arg);
    g_ptr_array_add(full, NULL);

    y720_cli_run("/usr/bin/sudo", (const gchar *const *)full->pdata,
                 input, timeout_seconds, cancellable, owner, callback, user_data);
    g_ptr_array_free(full, TRUE);
}
