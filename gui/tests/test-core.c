/* SPDX-License-Identifier: MIT */
#include <glib.h>
#include "y720-models.h"
#include "y720-temp.h"
#include "y720-cli.h"

static const gchar *fixture;
static Y720Result *reply;
static void complete(Y720Result *result, gpointer unused)
{
    (void)unused;
    reply = result;
}
static void wait_reply(void)
{
    gint64 end = g_get_monotonic_time() + 6000000;
    while (!reply && g_get_monotonic_time() < end) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
    g_assert_nonnull(reply);
}
static void protocol(void)
{
    const gchar *cases[] = {"ok", "invalid", "program", "version", "schema", "permission", "contradiction", "hang"};
    const gchar *codes[] = {NULL, "invalid_output", "invalid_output", "unsupported_version", "unsupported_schema", "permission", "invalid_output", "timeout"};
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        const gchar *args[] = {cases[i], NULL};
        reply = NULL;
        y720_cli_run(fixture, args, NULL, 1, NULL, NULL, complete, NULL);
        wait_reply();
        if (codes[i]) {
            g_assert_false(reply->ok);
            g_assert_cmpstr(reply->error_code, ==, codes[i]);
        } else g_assert_true(reply->ok);
        y720_result_free(reply);
    }
}
static gpointer cancel_thread(gpointer data)
{
    g_usleep(20000);
    g_cancellable_cancel(data);
    return NULL;
}
static void cancelled(void)
{
    GCancellable *cancel = g_cancellable_new();
    const gchar *args[] = {"hang", NULL};
    reply = NULL;
    y720_cli_run(fixture, args, NULL, 4, cancel, NULL, complete, NULL);
    GThread *thread = g_thread_new("cancel", cancel_thread, cancel);
    wait_reply();
    g_thread_join(thread);
    g_assert_true(reply->cancelled);
    g_assert_false(reply->ok);
    y720_result_free(reply);
    g_object_unref(cancel);
}
static void owner_destroyed(void)
{
    GObject *owner = g_object_new(G_TYPE_OBJECT, NULL);
    const gchar *args[] = {"ok", NULL};
    reply = NULL;
    y720_cli_run(fixture, args, NULL, 1, NULL, owner, complete, NULL);
    g_object_unref(owner);
    gint64 end = g_get_monotonic_time() + 250000;
    while (g_get_monotonic_time() < end) {
        g_main_context_iteration(NULL, FALSE);
        g_usleep(1000);
    }
    g_assert_null(reply);
}
static void temperatures(void)
{
    Y720FanStatus status = {.temperature_valid=TRUE, .temperature_mc=55001};
    g_assert_cmpint(y720_temp_display_from_status(&status), ==, 56);
    status.temperature_mc = 120000;
    g_assert_cmpint(y720_temp_display_from_status(&status), ==, 120);
    status.temperature_mc = 120001;
    g_assert_cmpint(y720_temp_display_from_status(&status), ==, -1);
    status.temperature_mc = 0;
    g_assert_cmpint(y720_temp_display_from_status(&status), ==, -1);
    status.temperature_valid = FALSE;
    g_assert_cmpint(y720_temp_display_from_status(&status), ==, -1);
}

static JsonObject *object_from_json(JsonParser *parser, const gchar *text)
{
    GError *error = NULL;
    g_assert_true(json_parser_load_from_data(parser, text, -1, &error));
    g_assert_no_error(error);
    return json_node_get_object(json_parser_get_root(parser));
}

static void model_bounds(void)
{
    JsonParser *parser = json_parser_new();
    Y720BacklightInfo light = {0};
    GError *error = NULL;
    const gchar *base = "{\"device\":\"mock\",\"hid_device\":\"mock\",\"driver\":\"hid-generic\","
        "\"descriptor_sha256\":\"mock\",\"report_bytes\":7,\"state_known\":false,\"profile\":null}";
    JsonObject *object = object_from_json(parser, base);
    g_assert_true(y720_backlight_info_parse(object, &light, &error));
    g_assert_no_error(error);
    g_assert_false(light.state_known);
    json_object_set_int_member(object, "report_bytes", G_MAXINT64);
    g_assert_false(y720_backlight_info_parse(object, &light, &error));
    g_clear_error(&error);
    json_object_set_int_member(object, "report_bytes", 7);
    JsonObject *profile = json_object_new();
    json_object_set_string_member(profile, "effect", "static");
    JsonArray *colors = json_array_new(), *levels = json_array_new();
    for (guint i = 0; i < 4; i++) {
        json_array_add_int_element(colors, 0);
        json_array_add_int_element(levels, 3);
    }
    json_object_set_array_member(profile, "colors", colors);
    json_object_set_array_member(profile, "brightness", levels);
    json_object_set_object_member(object, "profile", profile);
    g_assert_true(y720_backlight_info_parse(object, &light, &error));
    g_assert_no_error(error);
    json_object_set_string_member(profile, "effect", "unknown");
    g_assert_false(y720_backlight_info_parse(object, &light, &error));
    g_clear_error(&error);
    json_object_set_string_member(profile, "effect", "static");
    json_node_set_int(json_array_get_element(colors, 0), G_MAXINT64);
    g_assert_false(y720_backlight_info_parse(object, &light, &error));
    g_clear_error(&error);
    y720_backlight_info_clear(&light);

    Y720FanStatus fan = {0};
    object = object_from_json(parser,
        "{\"controls_available\":true,\"device\":\"mock\",\"driver_version\":\"" Y720_VERSION "\","
        "\"mode\":\"auto\",\"mode_raw\":2,\"target_rpm\":1500,\"pwm\":0,"
        "\"fan_rpm\":[1500,1500],\"diagnostics\":{}}" );
    g_assert_true(y720_fan_status_parse(object, &fan, &error));
    g_assert_no_error(error);
    g_assert_false(fan.temperature_valid);
    JsonObject *temps = json_object_new();
    json_object_set_int_member(temps, "millidegrees", 52000);
    json_object_set_string_member(temps, "source", "x86_pkg_temp");
    json_object_set_object_member(object, "temperature", temps);
    g_assert_true(y720_fan_status_parse(object, &fan, &error));
    g_assert_true(fan.temperature_valid);
    g_assert_cmpstr(fan.temperature_source, ==, "x86_pkg_temp");
    json_object_set_string_member(temps, "source", "untrusted");
    g_assert_false(y720_fan_status_parse(object, &fan, &error));
    g_clear_error(&error);
    json_object_set_string_member(temps, "source", "coretemp");
    json_object_set_int_member(object, "target_rpm", G_MAXINT64);
    g_assert_false(y720_fan_status_parse(object, &fan, &error));
    g_clear_error(&error);
    json_object_set_int_member(object, "target_rpm", 2100);
    json_object_set_string_member(object, "mode", "curve");
    json_object_set_int_member(object, "mode_raw", 3);
    JsonObject *curve = json_object_new();
    json_object_set_boolean_member(curve, "supported", TRUE);
    json_object_set_boolean_member(curve, "active", TRUE);
    json_object_set_string_member(curve, "engine", "linux-thermal");
    json_object_set_int_member(curve, "error", -61);
    json_object_set_null_member(curve, "temperature_millidegrees");
    JsonArray *points = json_array_new();
    const gint rpms[] = {1500,1800,2400,3200,4200,5100};
    for (guint i = 0; i < 6; i++) json_array_add_int_element(points, rpms[i]);
    json_object_set_array_member(curve, "points", points);
    json_object_set_object_member(object, "kernel_curve", curve);
    g_assert_true(y720_fan_status_parse(object, &fan, &error));
    g_assert_true(fan.kernel_curve_active);
    g_assert_cmpint(fan.kernel_curve_error, ==, -61);
    json_node_set_int(json_array_get_element(points, 3), G_MAXINT64);
    g_assert_false(y720_fan_status_parse(object, &fan, &error));
    g_clear_error(&error);
    json_node_set_int(json_array_get_element(points, 3), 3200);
    json_object_set_string_member(curve, "engine", "other");
    g_assert_false(y720_fan_status_parse(object, &fan, &error));
    g_clear_error(&error);
    y720_fan_status_clear(&fan);
    g_object_unref(parser);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_assert_cmpint(argc, >=, 2);
    fixture = argv[1];
    g_test_add_func("/gui/protocol", protocol);
    g_test_add_func("/gui/cancel-other-thread", cancelled);
    g_test_add_func("/gui/owner-destroyed", owner_destroyed);
    g_test_add_func("/curve/temperature-validity", temperatures);
    g_test_add_func("/gui/model-bounds", model_bounds);
    return g_test_run();
}
