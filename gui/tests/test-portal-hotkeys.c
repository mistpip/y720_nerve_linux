/* SPDX-License-Identifier: MIT */
/* Production client, private D-Bus portal double; never reads input or hardware. */
#include "y720-portal-hotkeys.h"
#include "y720-i18n.h"

#define ROOT "/org/freedesktop/portal/desktop"
#define IFACE "org.freedesktop.portal.GlobalShortcuts"
#define REQUEST "org.freedesktop.portal.Request"
#define SESSION "org.freedesktop.portal.Session"
#define NAME "org.freedesktop.portal.Desktop"
#define REGISTRY "org.freedesktop.host.portal.Registry"

static const gchar xml[] =
    "<node><interface name='" IFACE "'>"
    "<method name='CreateSession'><arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>"
    "<method name='BindShortcuts'><arg type='o' direction='in'/><arg type='a(sa{sv})' direction='in'/>"
    "<arg type='s' direction='in'/><arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>"
    "</interface><interface name='" REQUEST "'><method name='Close'/></interface>"
    "<interface name='" SESSION "'><method name='Close'/></interface>"
    "<interface name='" REGISTRY "'><method name='Register'><arg type='s' direction='in'/>"
    "<arg type='a{sv}' direction='in'/></method></interface></node>";

static GDBusConnection *server;
static GDBusNodeInfo *node;
static guint root_id, registry_id, calls, changed, fired, request_closes, session_closes;
static GArray *objects;
static gchar *session_path, *request_path;
static GVariant *bound;
static GDBusMethodInvocation *delayed;
static gboolean hold_create, hold_bind, deny, partial, unsupported, late_response;

static void spin(guint milliseconds)
{
    gint64 until = g_get_monotonic_time() + milliseconds * 1000;
    do {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    } while (g_get_monotonic_time() < until);
}

#define WAIT(condition) do { \
    gint64 deadline = g_get_monotonic_time() + 3000000; \
    while (!(condition) && g_get_monotonic_time() < deadline) spin(1); \
    g_assert_true(condition); \
} while (0)

static void emit(const gchar *path, const gchar *interface, const gchar *signal, GVariant *args)
{
    g_autoptr(GError) error = NULL;
    g_assert_true(g_dbus_connection_emit_signal(server, NULL, path, interface, signal, args, &error));
    g_assert_no_error(error);
    g_dbus_connection_flush_sync(server, NULL, NULL);
}

static GVariant *grants(void)
{
    GVariantBuilder array;
    g_variant_builder_init(&array, G_VARIANT_TYPE("a(sa{sv})"));
    GVariantIter iter;
    const gchar *id;
    GVariant *props;
    guint i = 0;
    g_variant_iter_init(&iter, bound);
    while (g_variant_iter_next(&iter, "(&s@a{sv})", &id, &props)) {
        const gchar *trigger;
        g_assert_true(g_variant_lookup(props, "preferred_trigger", "&s", &trigger));
        if (g_str_equal(id, "fan-toggle"))
            g_assert_cmpstr(trigger, ==, "CTRL+SHIFT+0");
        g_variant_unref(props);
        if (partial && i++ > 0) continue;
        GVariantBuilder actual;
        g_variant_builder_init(&actual, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&actual, "{sv}", "trigger_description",
                              g_variant_new_string("Desktop Ctrl+Shift+0"));
        g_variant_builder_add(&array, "(sa{sv})", id, &actual);
    }
    return g_variant_builder_end(&array);
}

static void respond(gboolean binding)
{
    GVariantBuilder results;
    g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
    if (binding)
        g_variant_builder_add(&results, "{sv}", "shortcuts", grants());
    else
        g_variant_builder_add(&results, "{sv}", "session_handle", g_variant_new_string(session_path));
    /* Deliberately emit Response BEFORE the method reply. */
    emit(request_path, REQUEST, "Response", g_variant_new("(ua{sv})", deny ? 1 : 0, &results));
}

static gboolean respond_later(gpointer data)
{
    respond(GPOINTER_TO_INT(data));
    return G_SOURCE_REMOVE;
}

static void method(GDBusConnection *bus, const gchar *sender, const gchar *path,
                   const gchar *interface, const gchar *name, GVariant *parameters,
                   GDBusMethodInvocation *invocation, gpointer data);
static const GDBusInterfaceVTable table = {.method_call = method};

static void register_path(const gchar *path, guint interface)
{
    g_autoptr(GError) error = NULL;
    guint id = g_dbus_connection_register_object(server, path, node->interfaces[interface],
                                                 &table, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(id, >, 0);
    g_array_append_val(objects, id);
}

static void method(GDBusConnection *bus, const gchar *sender, const gchar *path,
                   const gchar *interface, const gchar *name, GVariant *parameters,
                   GDBusMethodInvocation *invocation, gpointer data)
{
    (void)bus; (void)path; (void)data;
    if (g_str_equal(name, "Register")) {
        const gchar *app;
        g_variant_get_child(parameters, 0, "&s", &app);
        g_assert_cmpstr(app, ==, "org.y720.Control");
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (g_str_equal(name, "Close")) {
        if (g_str_equal(interface, REQUEST)) request_closes++;
        else session_closes++;
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    if (unsupported) {
        g_dbus_method_invocation_return_dbus_error(invocation,
            "org.freedesktop.DBus.Error.UnknownMethod", "GlobalShortcuts unsupported");
        return;
    }
    calls++;
    gboolean binding = g_str_equal(name, "BindShortcuts");
    g_autoptr(GVariant) options = g_variant_get_child_value(parameters, binding ? 3 : 0);
    const gchar *token, *session_token;
    g_assert_true(g_variant_lookup(options, "handle_token", "&s", &token));
    g_autofree gchar *peer = g_strdup(sender + 1);
    for (gchar *p = peer; *p; p++) if (*p == '.') *p = '_';
    g_free(request_path);
    request_path = g_strdup_printf(ROOT "/request/%s/%s", peer, token);
    register_path(request_path, 1);
    if (!binding) {
        g_assert_true(g_variant_lookup(options, "session_handle_token", "&s", &session_token));
        g_free(session_path);
        session_path = g_strdup_printf(ROOT "/session/%s/%s", peer, session_token);
        register_path(session_path, 2);
    } else {
        g_clear_pointer(&bound, g_variant_unref);
        bound = g_variant_get_child_value(parameters, 1);
    }
    if ((binding && hold_bind) || (!binding && hold_create)) {
        delayed = g_object_ref(invocation);
        return;
    }
    if (late_response)
        g_timeout_add(20, respond_later, GINT_TO_POINTER(binding));
    else
        respond(binding);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", request_path));
}

static void action(gpointer owner) { g_assert_true(G_IS_OBJECT(owner)); fired++; }
static void state(GObject *owner) { g_assert_true(G_IS_OBJECT(owner)); changed++; }
static Y720PortalHotkeys *start(GObject *owner)
{
    Y720PortalBinding bindings[] = {
        {"light-next", "Light profile", "<Control><Alt>p", action},
        {"fan-toggle", "Toggle auto/boost", "<Control><Shift>0", action},
    };
    g_autoptr(GError) error = NULL;
    Y720PortalHotkeys *self = y720_portal_hotkeys_new(bindings, 2, owner, state, &error);
    g_assert_no_error(error);
    g_assert_nonnull(self);
    g_assert_true(y720_portal_hotkeys_pending(self));
    return self;
}

static void key(const gchar *session, const gchar *id, gboolean down)
{
    emit(ROOT, IFACE, down ? "Activated" : "Deactivated",
          g_variant_new("(ost@a{sv})", session, id, (guint64)123,
                        g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)));
    spin(25);
}

static void cleanup(void)
{
    spin(40);
    for (guint i = 0; i < objects->len; i++)
        g_dbus_connection_unregister_object(server, g_array_index(objects, guint, i));
    g_array_set_size(objects, 0);
    g_clear_pointer(&bound, g_variant_unref);
    g_clear_pointer(&session_path, g_free);
    g_clear_pointer(&request_path, g_free);
    g_clear_object(&delayed);
    hold_create = hold_bind = deny = partial = unsupported = late_response = FALSE;
    calls = changed = fired = request_closes = session_closes = 0;
}

static void test_normal(void)
{
    g_autoptr(GObject) owner = g_object_new(G_TYPE_OBJECT, NULL);
    g_autoptr(Y720PortalHotkeys) self = start(owner);
    WAIT(y720_portal_hotkeys_active(self));
    g_assert_false(y720_portal_hotkeys_pending(self));
    g_assert_nonnull(g_strstr_len(y720_portal_hotkeys_status(self), -1, "Desktop Ctrl+Shift+0"));
    key(ROOT "/session/unrelated", "fan-toggle", TRUE);
    key(session_path, "unknown", TRUE);
    g_assert_cmpuint(fired, ==, 0);
    key(session_path, "fan-toggle", TRUE);
    key(session_path, "fan-toggle", TRUE);
    g_assert_cmpuint(fired, ==, 1);
    key(session_path, "fan-toggle", FALSE);
    key(session_path, "fan-toggle", TRUE);
    g_assert_cmpuint(fired, ==, 2);
    partial = TRUE;
    emit(ROOT, IFACE, "ShortcutsChanged", g_variant_new("(o@a(sa{sv}))", session_path, grants()));
    WAIT(!y720_portal_hotkeys_active(self));
    key(session_path, "fan-toggle", TRUE);
    g_assert_cmpuint(fired, ==, 2); /* Revoked shortcut is no longer executable. */
    y720_portal_hotkeys_stop(self);
    WAIT(session_closes > 0);
    key(session_path, "light-next", TRUE);
    g_assert_cmpuint(fired, ==, 2);
    cleanup();
}

static void test_denied(void)
{
    deny = TRUE;
    g_autoptr(GObject) owner = g_object_new(G_TYPE_OBJECT, NULL);
    g_autoptr(Y720PortalHotkeys) self = start(owner);
    WAIT(!y720_portal_hotkeys_pending(self));
    g_assert_false(y720_portal_hotkeys_active(self));
    g_assert_cmpuint(calls, ==, 1);
    g_assert_nonnull(g_strstr_len(y720_portal_hotkeys_status(self), -1, "cancelled"));
    y720_portal_hotkeys_stop(self);
    cleanup();
}

static void test_later_response(void)
{
    late_response = TRUE;
    g_autoptr(GObject) owner = g_object_new(G_TYPE_OBJECT, NULL);
    g_autoptr(Y720PortalHotkeys) self = start(owner);
    WAIT(y720_portal_hotkeys_active(self));
    key(session_path, "fan-toggle", TRUE);
    g_assert_cmpuint(fired, ==, 1);
    y720_portal_hotkeys_stop(self);
    cleanup();
}

static void test_service_lost(void)
{
    g_autoptr(GObject) owner = g_object_new(G_TYPE_OBJECT, NULL);
    g_autoptr(Y720PortalHotkeys) self = start(owner);
    WAIT(y720_portal_hotkeys_active(self));
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(server, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "ReleaseName", g_variant_new("(s)", NAME),
        NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    g_assert_nonnull(reply);
    WAIT(!y720_portal_hotkeys_active(self));
    g_assert_nonnull(g_strstr_len(y720_portal_hotkeys_status(self), -1, "portal unavailable"));
    y720_portal_hotkeys_stop(self);
    g_clear_pointer(&reply, g_variant_unref);
    reply = g_dbus_connection_call_sync(server, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", NAME, 0u),
        NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    g_assert_nonnull(reply);
    cleanup();
}

static void test_unavailable(void)
{
    unsupported = TRUE;
    g_autoptr(GObject) owner = g_object_new(G_TYPE_OBJECT, NULL);
    g_autoptr(Y720PortalHotkeys) self = start(owner);
    WAIT(!y720_portal_hotkeys_pending(self));
    g_assert_false(y720_portal_hotkeys_active(self));
    g_assert_nonnull(g_strstr_len(y720_portal_hotkeys_status(self), -1, "unsupported"));
    y720_portal_hotkeys_stop(self);
    cleanup();
}

static void test_closed(void)
{
    g_autoptr(GObject) owner = g_object_new(G_TYPE_OBJECT, NULL);
    g_autoptr(Y720PortalHotkeys) self = start(owner);
    WAIT(y720_portal_hotkeys_active(self));
    emit(session_path, SESSION, "Closed", g_variant_new("()"));
    WAIT(!y720_portal_hotkeys_active(self));
    g_assert_nonnull(g_strstr_len(y720_portal_hotkeys_status(self), -1, "closed"));
    y720_portal_hotkeys_stop(self);
    cleanup();
}

static void test_owner_lifetime(void)
{
    GObject *owner = g_object_new(G_TYPE_OBJECT, NULL);
    g_autoptr(Y720PortalHotkeys) self = start(owner);
    WAIT(y720_portal_hotkeys_active(self));
    g_object_unref(owner);
    key(session_path, "fan-toggle", TRUE);
    g_assert_cmpuint(fired, ==, 0);
    y720_portal_hotkeys_stop(self);
    cleanup();
}

static void test_late_reply(void)
{
    for (guint binding = 0; binding < 2; binding++) {
        hold_create = !binding; hold_bind = binding;
        g_autoptr(GObject) owner = g_object_new(G_TYPE_OBJECT, NULL);
        Y720PortalHotkeys *self = start(owner);
        WAIT(delayed != NULL);
        y720_portal_hotkeys_stop(self);
        g_object_unref(self); /* No late callbacks may use the destroyed application. */
        g_clear_object(&owner);
        guint before = changed;
        respond(binding);
        g_dbus_method_invocation_return_value(delayed, g_variant_new("(o)", request_path));
        g_clear_object(&delayed);
        WAIT(request_closes > 0 && session_closes > 0);
        spin(50);
        g_assert_cmpuint(changed, ==, before);
        g_assert_cmpuint(fired, ==, 0);
        cleanup();
    }
    /* Stop before asynchronous bus discovery finishes as well. */
    g_autoptr(GObject) owner = g_object_new(G_TYPE_OBJECT, NULL);
    Y720PortalHotkeys *self = start(owner);
    y720_portal_hotkeys_stop(self);
    g_object_unref(self);
    spin(50);
    g_assert_cmpuint(calls, ==, 0);
    cleanup();
}

static void test_trigger(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *trigger = y720_portal_trigger("<Control><Shift>0", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(trigger, ==, "CTRL+SHIFT+0");
    g_free(g_steal_pointer(&trigger));
    trigger = y720_portal_trigger("<Super><Alt>F8", &error);
    g_assert_cmpstr(trigger, ==, "ALT+LOGO+F8");
    g_assert_null(y720_portal_trigger("keycode:236", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_clear_error(&error);
    g_assert_null(y720_portal_trigger("evdev:228", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_clear_error(&error);
    g_autoptr(GObject) owner = g_object_new(G_TYPE_OBJECT, NULL);
    Y720PortalBinding duplicate[] = {
        {"a", "a", "<Control>p", action}, {"b", "b", "<Primary>p", action},
    };
    g_assert_null(y720_portal_hotkeys_new(duplicate, 2, owner, state, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    y720_i18n_set_language("en");
    g_autoptr(GTestDBus) daemon = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(daemon);
    server = g_dbus_connection_new_for_address_sync(g_test_dbus_get_bus_address(daemon),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, NULL);
    g_assert_nonnull(server);
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(server, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", g_variant_new("(su)", NAME, 0u),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    g_assert_nonnull(reply);
    node = g_dbus_node_info_new_for_xml(xml, NULL);
    root_id = g_dbus_connection_register_object(server, ROOT, node->interfaces[0], &table, NULL, NULL, NULL);
    registry_id = g_dbus_connection_register_object(server, ROOT, node->interfaces[3], &table, NULL, NULL, NULL);
    objects = g_array_new(FALSE, FALSE, sizeof(guint));
    g_test_add_func("/portal/trigger", test_trigger);
    g_test_add_func("/portal/authorization-early-response-remap-repeat-stop", test_normal);
    g_test_add_func("/portal/denied", test_denied);
    g_test_add_func("/portal/response-after-method-reply", test_later_response);
    g_test_add_func("/portal/service-lost", test_service_lost);
    g_test_add_func("/portal/unsupported", test_unavailable);
    g_test_add_func("/portal/session-closed", test_closed);
    g_test_add_func("/portal/owner-destroyed", test_owner_lifetime);
    g_test_add_func("/portal/cancel-late-reply", test_late_reply);
    int status = g_test_run();
    g_dbus_connection_unregister_object(server, root_id);
    g_dbus_connection_unregister_object(server, registry_id);
    g_dbus_connection_close_sync(server, NULL, NULL);
    g_clear_object(&server);
    g_array_unref(objects);
    g_dbus_node_info_unref(node);
    spin(30);
    g_test_dbus_down(daemon);
    return status;
}
