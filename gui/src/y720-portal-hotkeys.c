/* SPDX-License-Identifier: MIT */
#include "y720-portal-hotkeys.h"
#include "y720-i18n.h"
#include <gtk/gtk.h>

#define PORTAL "org.freedesktop.portal.Desktop"
#define ROOT "/org/freedesktop/portal/desktop"
#define SHORTCUTS "org.freedesktop.portal.GlobalShortcuts"
#define REQUEST "org.freedesktop.portal.Request"
#define SESSION "org.freedesktop.portal.Session"
#define REGISTRY "org.freedesktop.host.portal.Registry"
#define LIMIT 4

typedef struct {
    gchar *id, *description, *trigger, *actual;
    Y720HotkeyFunc callback;
    gboolean granted, pressed;
} Shortcut;

struct _Y720PortalHotkeys {
    GObject parent_instance;
    GWeakRef owner;
    Y720HotkeyStateFunc changed;
    GDBusConnection *bus;
    GCancellable *cancel;
    gchar *peer, *session, *request, *status;
    guint watch, responses, signals, closed;
    guint serial, count;
    gboolean stopped, pending, binding, replied, active;
    GVariant *early_response;
    Shortcut shortcuts[LIMIT];
};
G_DEFINE_TYPE(Y720PortalHotkeys, y720_portal_hotkeys, G_TYPE_OBJECT)

typedef struct {
    Y720PortalHotkeys *self;
    guint serial;
    gchar *session;
} Call;

gchar *y720_portal_trigger(const gchar *accelerator, GError **error)
{
    guint key = 0;
    GdkModifierType mods = 0;
    const guint allowed = GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_ALT_MASK | GDK_SUPER_MASK;
    if (!accelerator || !gtk_accelerator_parse(accelerator, &key, &mods) ||
        !key || (mods & ~allowed)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            y720_tr("Wayland 需要按键名称，请重新录制旧的 X11 键码"));
        return NULL;
    }
    const gchar *name = gdk_keyval_name(gdk_keyval_to_lower(key));
    if (!name || !*name)
        goto invalid;
    for (const gchar *p = name; *p; p++)
        if (!g_ascii_isalnum(*p) && *p != '_')
            goto invalid;
    return g_strconcat(mods & GDK_CONTROL_MASK ? "CTRL+" : "",
                       mods & GDK_ALT_MASK ? "ALT+" : "",
                       mods & GDK_SHIFT_MASK ? "SHIFT+" : "",
                       mods & GDK_SUPER_MASK ? "LOGO+" : "", name, NULL);
invalid:
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        y720_tr("无法解析快捷键"));
    return NULL;
}

static void notify(Y720PortalHotkeys *self, const gchar *message)
{
    g_free(self->status);
    self->status = g_strdup(message);
    g_autoptr(GObject) owner = g_weak_ref_get(&self->owner);
    if (!self->stopped && owner && self->changed)
        self->changed(owner);
}

static void close_path(Y720PortalHotkeys *self, const gchar *path, const gchar *interface)
{
    if (self->bus && self->peer && path)
        g_dbus_connection_call(self->bus, self->peer, path, interface, "Close", NULL,
                               NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000,
                               NULL, NULL, NULL);
}

static void fail(Y720PortalHotkeys *self, const gchar *message)
{
    self->pending = self->active = FALSE;
    close_path(self, self->request, REQUEST);
    close_path(self, self->session, SESSION);
    g_clear_pointer(&self->request, g_free);
    g_clear_pointer(&self->session, g_free);
    g_clear_pointer(&self->early_response, g_variant_unref);
    self->serial++;
    for (guint i = 0; i < self->count; i++)
        self->shortcuts[i].granted = self->shortcuts[i].pressed = FALSE;
    notify(self, message);
}

static void call_free(Call *call)
{
    g_object_unref(call->self);
    g_free(call->session);
    g_free(call);
}

static void update_bindings(Y720PortalHotkeys *self, GVariant *list)
{
    for (guint i = 0; i < self->count; i++) {
        self->shortcuts[i].granted = self->shortcuts[i].pressed = FALSE;
        g_clear_pointer(&self->shortcuts[i].actual, g_free);
    }
    GVariantIter iter;
    const gchar *id;
    GVariant *properties;
    g_variant_iter_init(&iter, list);
    while (g_variant_iter_next(&iter, "(&s@a{sv})", &id, &properties)) {
        for (guint i = 0; i < self->count; i++) {
            Shortcut *s = &self->shortcuts[i];
            if (g_str_equal(s->id, id)) {
                const gchar *actual = NULL;
                s->granted = TRUE;
                g_variant_lookup(properties, "trigger_description", "&s", &actual);
                g_free(s->actual);
                s->actual = g_strdup(actual ? actual : y720_tr("由桌面分配"));
            }
        }
        g_variant_unref(properties);
    }
    guint granted = 0;
    g_autoptr(GString) text = g_string_new(NULL);
    for (guint i = 0; i < self->count; i++)
        granted += self->shortcuts[i].granted;
    self->pending = FALSE;
    self->active = granted == self->count;
    g_string_append(text, y720_tr(self->active ? "Wayland 桌面快捷键已启用" :
                                  "Wayland 未授权全部快捷键"));
    for (guint i = 0; i < self->count; i++) {
        Shortcut *s = &self->shortcuts[i];
        if (s->granted)
            g_string_append_printf(text, " · %s: %s", s->description, s->actual);
    }
    notify(self, text->str);
}

static void begin_request(Y720PortalHotkeys *self, gboolean binding);

static void complete_response(Y720PortalHotkeys *self, GVariant *response)
{
    guint code;
    g_autoptr(GVariant) results = NULL;
    g_variant_get(response, "(u@a{sv})", &code, &results);
    g_clear_pointer(&self->request, g_free);
    if (code != 0) {
        fail(self, y720_tr(code == 1 ? "Wayland 快捷键授权已取消" :
                                      "Wayland 桌面无法注册快捷键"));
        return;
    }
    if (!self->binding) {
        const gchar *path = NULL;
        if (!g_variant_lookup(results, "session_handle", "&s", &path) ||
            !g_variant_is_object_path(path) || !g_str_has_prefix(path, ROOT "/session/")) {
            fail(self, y720_tr("Wayland 桌面返回了无效的快捷键会话"));
            return;
        }
        g_free(self->session);
        self->session = g_strdup(path);
        begin_request(self, TRUE);
    } else {
        g_autoptr(GVariant) shortcuts = g_variant_lookup_value(results, "shortcuts",
                                                              G_VARIANT_TYPE("a(sa{sv})"));
        if (!shortcuts)
            fail(self, y720_tr("Wayland 桌面未返回快捷键授权结果"));
        else
            update_bindings(self, shortcuts);
    }
}

static void response_signal(GDBusConnection *bus, const gchar *sender, const gchar *path,
                            const gchar *interface, const gchar *signal,
                            GVariant *parameters, gpointer data)
{
    (void)bus; (void)sender; (void)interface; (void)signal;
    g_autoptr(Y720PortalHotkeys) self = g_object_ref(data);
    if (self->stopped || !self->request || !g_str_equal(path, self->request) ||
        !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ua{sv})")))
        return;
    if (!self->replied) {
        g_clear_pointer(&self->early_response, g_variant_unref);
        self->early_response = g_variant_ref(parameters);
    } else {
        complete_response(self, parameters);
    }
}

static void request_reply(GObject *object, GAsyncResult *result, gpointer data)
{
    Call *call = data;
    Y720PortalHotkeys *self = call->self;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object),
                                                            result, &error);
    if (self->stopped || call->serial != self->serial) {
        if (reply) {
            const gchar *path;
            g_variant_get(reply, "(&o)", &path);
            close_path(self, path, REQUEST);
            close_path(self, call->session, SESSION);
        }
        call_free(call);
        return;
    }
    if (!reply) {
        g_autofree gchar *message = g_strdup_printf(y720_tr("Wayland 快捷键不可用：%s"),
                                                     error->message);
        fail(self, message);
    } else {
        const gchar *path;
        g_variant_get(reply, "(&o)", &path);
        /* New portals use handle_token. Accept legacy returned paths as well. */
        if (g_strcmp0(path, self->request)) {
            g_free(self->request);
            self->request = g_strdup(path);
            g_clear_pointer(&self->early_response, g_variant_unref);
        }
        self->replied = TRUE;
        if (self->early_response) {
            g_autoptr(GVariant) response = g_steal_pointer(&self->early_response);
            complete_response(self, response);
        }
    }
    call_free(call);
}

static gchar *token(void)
{
    gchar *value = g_uuid_string_random();
    for (gchar *p = value; *p; p++)
        if (*p == '-') *p = '_';
    return value;
}

static void begin_request(Y720PortalHotkeys *self, gboolean binding)
{
    g_autofree gchar *request_token = token();
    g_autofree gchar *session_token = token();
    g_autofree gchar *sender = g_strdup(g_dbus_connection_get_unique_name(self->bus) + 1);
    for (gchar *p = sender; *p; p++)
        if (*p == '.') *p = '_';
    g_free(self->request);
    self->request = g_strdup_printf(ROOT "/request/%s/%s", sender, request_token);
    self->binding = binding;
    self->replied = FALSE;
    self->serial++;

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(request_token));
    GVariant *parameters;
    if (!binding) {
        g_free(self->session);
        self->session = g_strdup_printf(ROOT "/session/%s/%s", sender, session_token);
        g_variant_builder_add(&options, "{sv}", "session_handle_token",
                              g_variant_new_string(session_token));
        parameters = g_variant_new("(a{sv})", &options);
    } else {
        GVariantBuilder shortcuts;
        g_variant_builder_init(&shortcuts, G_VARIANT_TYPE("a(sa{sv})"));
        for (guint i = 0; i < self->count; i++) {
            Shortcut *s = &self->shortcuts[i];
            GVariantBuilder properties;
            g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&properties, "{sv}", "description", g_variant_new_string(s->description));
            g_variant_builder_add(&properties, "{sv}", "preferred_trigger", g_variant_new_string(s->trigger));
            g_variant_builder_add(&shortcuts, "(sa{sv})", s->id, &properties);
        }
        /* No toplevel exists during tray startup. An empty parent is allowed. */
        parameters = g_variant_new("(oa(sa{sv})sa{sv})", self->session, &shortcuts, "", &options);
    }
    Call *call = g_new0(Call, 1);
    call->self = g_object_ref(self);
    call->serial = self->serial;
    call->session = g_strdup(self->session);
    /* Do not cancel the method reply: a late-created session still needs Close. */
    g_dbus_connection_call(self->bus, self->peer, ROOT, SHORTCUTS,
                           binding ? "BindShortcuts" : "CreateSession", parameters,
                           G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 10000,
                           NULL, request_reply, call);
}

static void shortcut_signal(GDBusConnection *bus, const gchar *sender, const gchar *path,
                            const gchar *interface, const gchar *signal,
                            GVariant *parameters, gpointer data)
{
    (void)bus; (void)sender; (void)path; (void)interface;
    g_autoptr(Y720PortalHotkeys) self = g_object_ref(data);
    if (self->stopped || !self->session)
        return;
    if (g_str_equal(signal, "ShortcutsChanged")) {
        if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(oa(sa{sv}))"))) return;
        const gchar *session;
        g_autoptr(GVariant) list = NULL;
        g_variant_get(parameters, "(&o@a(sa{sv}))", &session, &list);
        if (g_str_equal(session, self->session) && !self->pending)
            update_bindings(self, list);
        return;
    }
    if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(osta{sv})"))) return;
    const gchar *session, *id;
    guint64 timestamp;
    g_autoptr(GVariant) options = NULL;
    g_variant_get(parameters, "(&o&st@a{sv})", &session, &id, &timestamp, &options);
    if (!g_str_equal(session, self->session) || self->pending)
        return;
    for (guint i = 0; i < self->count; i++) {
        Shortcut *s = &self->shortcuts[i];
        if (!s->granted || !g_str_equal(s->id, id)) continue;
        if (g_str_equal(signal, "Deactivated")) {
            s->pressed = FALSE;
        } else if (g_str_equal(signal, "Activated") && !s->pressed) {
            s->pressed = TRUE;
            g_autoptr(GObject) owner = g_weak_ref_get(&self->owner);
            if (owner) s->callback(owner);
        }
        break;
    }
}

static void session_closed(GDBusConnection *bus, const gchar *sender, const gchar *path,
                            const gchar *interface, const gchar *signal,
                            GVariant *parameters, gpointer data)
{
    (void)bus; (void)sender; (void)interface; (void)signal; (void)parameters;
    g_autoptr(Y720PortalHotkeys) self = g_object_ref(data);
    if (!self->stopped && self->session && g_str_equal(path, self->session))
        fail(self, y720_tr("Wayland 快捷键会话已关闭，请重新启用"));
}

static void registered(GObject *object, GAsyncResult *result, gpointer data)
{
    g_autoptr(Y720PortalHotkeys) self = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
    if (self->stopped) return;
    /* Registry is absent on older portals. Modern native packages must identify
     * their own private connection before any portal method, not GTK's bus. */
    if (!reply && !g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD) &&
        !g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE)) {
        g_autofree gchar *message = g_strdup_printf(y720_tr("Wayland 快捷键不可用：%s"), error->message);
        fail(self, message);
        return;
    }
    begin_request(self, FALSE);
}

static void appeared(GDBusConnection *bus, const gchar *name, const gchar *peer, gpointer data)
{
    (void)name;
    g_autoptr(Y720PortalHotkeys) self = g_object_ref(data);
    if (self->stopped || self->peer) return;
    self->peer = g_strdup(peer);
    /* Subscribe before CreateSession: Response may precede its method reply. */
    self->responses = g_dbus_connection_signal_subscribe(bus, peer, REQUEST, "Response",
                              NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, response_signal, self, NULL);
    self->signals = g_dbus_connection_signal_subscribe(bus, peer, SHORTCUTS, NULL,
                              ROOT, NULL, G_DBUS_SIGNAL_FLAGS_NONE, shortcut_signal, self, NULL);
    self->closed = g_dbus_connection_signal_subscribe(bus, peer, SESSION, "Closed",
                              NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, session_closed, self, NULL);
    g_dbus_connection_call(bus, peer, ROOT, REGISTRY, "Register",
        g_variant_new("(s@a{sv})", "org.y720.Control",
                      g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)),
        G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE, 5000, self->cancel,
        registered, g_object_ref(self));
}

static void vanished(GDBusConnection *bus, const gchar *name, gpointer data)
{
    (void)bus; (void)name;
    g_autoptr(Y720PortalHotkeys) self = g_object_ref(data);
    if (!self->stopped)
        fail(self, y720_tr("Wayland 快捷键 Portal 不可用，请检查桌面后端"));
}

static void connect_bus(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; (void)data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, cancel, &error);
    GDBusConnection *bus = address ? g_dbus_connection_new_for_address_sync(address,
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, cancel, &error) : NULL;
    if (bus) {
        g_dbus_connection_set_exit_on_close(bus, FALSE);
        g_task_return_pointer(task, bus, g_object_unref);
    } else {
        g_task_return_error(task, g_steal_pointer(&error));
    }
}

static void bus_ready(GObject *object, GAsyncResult *result, gpointer data)
{
    (void)data;
    Y720PortalHotkeys *self = Y720_PORTAL_HOTKEYS(object);
    g_autoptr(GError) error = NULL;
    GDBusConnection *bus = g_task_propagate_pointer(G_TASK(result), &error);
    if (self->stopped) {
        if (bus) g_dbus_connection_close(bus, NULL, NULL, NULL);
        g_clear_object(&bus);
        return;
    }
    if (!bus) {
        fail(self, y720_tr("无法连接桌面会话总线"));
        return;
    }
    self->bus = bus;
    self->watch = g_bus_watch_name_on_connection(bus, PORTAL, G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                                                appeared, vanished, self, NULL);
}

static void flushed(GObject *object, GAsyncResult *result, gpointer data)
{
    (void)object;
    g_autoptr(GDBusConnection) bus = data;
    g_dbus_connection_flush_finish(bus, result, NULL);
    /* Disconnect also releases a session created after cancellation. */
    g_dbus_connection_close(bus, NULL, NULL, NULL);
}

void y720_portal_hotkeys_stop(Y720PortalHotkeys *self)
{
    if (!self || self->stopped) return;
    self->stopped = TRUE;
    self->pending = self->active = FALSE;
    g_cancellable_cancel(self->cancel);
    if (self->watch) { g_bus_unwatch_name(self->watch); self->watch = 0; }
    if (self->bus) {
        if (self->responses) g_dbus_connection_signal_unsubscribe(self->bus, self->responses);
        if (self->signals) g_dbus_connection_signal_unsubscribe(self->bus, self->signals);
        if (self->closed) g_dbus_connection_signal_unsubscribe(self->bus, self->closed);
    }
    self->responses = self->signals = self->closed = 0;
    close_path(self, self->request, REQUEST);
    close_path(self, self->session, SESSION);
    if (self->bus)
        g_dbus_connection_flush(self->bus, NULL, flushed, g_object_ref(self->bus));
}

static void dispose(GObject *object)
{
    Y720PortalHotkeys *self = Y720_PORTAL_HOTKEYS(object);
    y720_portal_hotkeys_stop(self);
    g_clear_object(&self->cancel);
    g_clear_object(&self->bus);
    G_OBJECT_CLASS(y720_portal_hotkeys_parent_class)->dispose(object);
}

static void finalize(GObject *object)
{
    Y720PortalHotkeys *self = Y720_PORTAL_HOTKEYS(object);
    g_weak_ref_clear(&self->owner);
    g_free(self->peer); g_free(self->session); g_free(self->request); g_free(self->status);
    g_clear_pointer(&self->early_response, g_variant_unref);
    for (guint i = 0; i < self->count; i++) {
        Shortcut *s = &self->shortcuts[i];
        g_free(s->id); g_free(s->description); g_free(s->trigger); g_free(s->actual);
    }
    G_OBJECT_CLASS(y720_portal_hotkeys_parent_class)->finalize(object);
}

static void y720_portal_hotkeys_class_init(Y720PortalHotkeysClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = dispose;
    G_OBJECT_CLASS(klass)->finalize = finalize;
}

static void y720_portal_hotkeys_init(Y720PortalHotkeys *self)
{
    g_weak_ref_init(&self->owner, NULL);
    self->cancel = g_cancellable_new();
    self->status = g_strdup(y720_tr("正在等待 Wayland 桌面授权快捷键…"));
    self->pending = TRUE;
}

Y720PortalHotkeys *y720_portal_hotkeys_new(const Y720PortalBinding *bindings, guint count,
                                         GObject *owner, Y720HotkeyStateFunc changed, GError **error)
{
    g_return_val_if_fail(G_IS_OBJECT(owner), NULL);
    if (!bindings || !count || count > LIMIT) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            y720_tr("快捷键数量无效"));
        return NULL;
    }
    g_autoptr(Y720PortalHotkeys) self = g_object_new(Y720_TYPE_PORTAL_HOTKEYS, NULL);
    g_weak_ref_set(&self->owner, owner);
    self->changed = changed;
    for (guint i = 0; i < count; i++) {
        const Y720PortalBinding *b = &bindings[i];
        if (!b->id || !*b->id || !b->description || !b->callback) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                y720_tr("无法解析快捷键"));
            return NULL;
        }
        for (guint j = 0; j < i; j++) {
            if (g_str_equal(b->id, bindings[j].id) ||
                g_strcmp0(b->accelerator, bindings[j].accelerator) == 0) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                    y720_tr("两个快捷键重复"));
                return NULL;
            }
        }
        Shortcut *s = &self->shortcuts[self->count++];
        s->id = g_strdup(b->id);
        s->description = g_strdup(b->description);
        s->callback = b->callback;
        s->trigger = y720_portal_trigger(b->accelerator, error);
        if (!s->trigger) return NULL;
        for (guint j = 0; j < i; j++) {
            if (g_str_equal(s->trigger, self->shortcuts[j].trigger)) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                    y720_tr("两个快捷键重复"));
                return NULL;
            }
        }
    }
    g_autoptr(GTask) task = g_task_new(self, self->cancel, bus_ready, NULL);
    g_task_set_check_cancellable(task, FALSE);
    g_task_set_return_on_cancel(task, FALSE);
    g_task_run_in_thread(task, connect_bus);
    return g_steal_pointer(&self);
}

gboolean y720_portal_hotkeys_active(Y720PortalHotkeys *self) { return self && self->active; }
gboolean y720_portal_hotkeys_pending(Y720PortalHotkeys *self) { return self && self->pending; }
const gchar *y720_portal_hotkeys_status(Y720PortalHotkeys *self) { return self ? self->status : NULL; }
