/* SPDX-License-Identifier: MIT */
#include "y720-tray.h"

#include <gtk/gtk.h>
#include <string.h>
#include <unistd.h>

static const gchar *sni_xml =
    "<node>"
    " <interface name='org.kde.StatusNotifierItem'>"
    "  <method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "  <method name='Activate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "  <method name='SecondaryActivate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "  <method name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method>"
    "  <property name='Category' type='s' access='read'/>"
    "  <property name='Id' type='s' access='read'/>"
    "  <property name='Title' type='s' access='read'/>"
    "  <property name='Status' type='s' access='read'/>"
    "  <property name='IconName' type='s' access='read'/>"
    "  <property name='IconPixmap' type='a(iiay)' access='read'/>"
    "  <property name='OverlayIconName' type='s' access='read'/>"
    "  <property name='OverlayIconPixmap' type='a(iiay)' access='read'/>"
    "  <property name='AttentionIconName' type='s' access='read'/>"
    "  <property name='AttentionIconPixmap' type='a(iiay)' access='read'/>"
    "  <property name='AttentionMovieName' type='s' access='read'/>"
    "  <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "  <property name='ItemIsMenu' type='b' access='read'/>"
    "  <property name='Menu' type='o' access='read'/>"
    "  <property name='WindowId' type='i' access='read'/>"
    "  <signal name='NewIcon'/>"
    "  <signal name='NewStatus'><arg type='s'/></signal>"
    "  <signal name='NewTitle'/>"
    "  <signal name='NewToolTip'/>"
    " </interface>"
    "</node>";

static const gchar *menu_xml =
    "<node>"
    " <interface name='com.canonical.dbusmenu'>"
    "  <method name='GetLayout'>"
    "   <arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='as' direction='in'/>"
    "   <arg type='u' direction='out'/><arg type='(ia{sv}av)' direction='out'/>"
    "  </method>"
    "  <method name='GetGroupProperties'>"
    "   <arg type='ai' direction='in'/><arg type='as' direction='in'/>"
    "   <arg type='a(ia{sv})' direction='out'/>"
    "  </method>"
    "  <method name='GetProperty'>"
    "   <arg type='i' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='out'/>"
    "  </method>"
    "  <method name='Event'>"
    "   <arg type='i' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='in'/><arg type='u' direction='in'/>"
    "  </method>"
    "  <method name='EventGroup'>"
    "   <arg type='a(isvu)' direction='in'/><arg type='ai' direction='out'/>"
    "  </method>"
    "  <method name='AboutToShow'><arg type='i' direction='in'/><arg type='b' direction='out'/></method>"
    "  <method name='AboutToShowGroup'>"
    "   <arg type='ai' direction='in'/><arg type='ai' direction='out'/><arg type='ai' direction='out'/>"
    "  </method>"
    "  <property name='Version' type='u' access='read'/>"
    "  <property name='TextDirection' type='s' access='read'/>"
    "  <property name='Status' type='s' access='read'/>"
    "  <property name='IconThemePath' type='as' access='read'/>"
    "  <signal name='LayoutUpdated'><arg type='u'/><arg type='i'/></signal>"
    "  <signal name='ItemsPropertiesUpdated'><arg type='a(ia{sv})'/><arg type='a(ias)'/></signal>"
    " </interface>"
    "</node>";

struct _Y720Tray {
    GDBusConnection *bus;
    Y720TrayActionFunc callback;
    gpointer user_data;
    gchar *service_name;
    gchar *object_path;
    gboolean watcher_present;
    gboolean name_owned;
    gboolean window_visible;
    gboolean controls;
    gboolean hotkey_ok;
    gboolean extreme;
    GPtrArray *profiles;
    guint profile_selected;
    gboolean curve_enabled;
    gchar *mode;
    gint rpm;
    guint sni_registration;
    guint menu_registration;
    guint owner_id;
    guint revision;
};

enum {
    ITEM_ROOT = 0,
    ITEM_TOGGLE = 1,
    ITEM_SEP1 = 2,
    ITEM_AUTO = 3,
    ITEM_BOOST = 4,
    ITEM_SEP2 = 5,
    ITEM_SEP3 = 7,
    ITEM_SETTINGS = 8,
    ITEM_QUIT = 9,
    ITEM_PROFILES = 11,
    ITEM_CURVE = 12,
    ITEM_PROFILE_BASE = 100,
};

static void emit_layout_updated(Y720Tray *tray)
{
    tray->revision++;
    g_dbus_connection_emit_signal(tray->bus, NULL, "/MenuBar",
                                  "com.canonical.dbusmenu", "LayoutUpdated",
                                  g_variant_new("(ui)", tray->revision, 0), NULL);
}

static const gchar *mode_label(const gchar *mode)
{
    if (g_strcmp0(mode, "auto") == 0)
        return "自动";
    if (g_strcmp0(mode, "boost") == 0)
        return "极致散热";
    if (g_strcmp0(mode, "curve") == 0)
        return "自定义";
    return "只读";
}

static gboolean mode_is(const Y720Tray *tray, const gchar *mode)
{
    return g_strcmp0(tray->mode, mode) == 0;
}

static void add_prop(GVariantBuilder *props, const gchar *key, GVariant *value)
{
    g_variant_builder_add(props, "{sv}", key, value);
}

static GVariant *icon_pixmap(Y720Tray *tray)
{
    const gchar *path = tray->extreme
        ? "/org/y720/Control/icons/tray-active-48.png"
        : "/org/y720/Control/icons/tray-idle-48.png";
    GdkTexture *texture = gdk_texture_new_from_resource(path);
    if (!texture)
        return g_variant_new_array(G_VARIANT_TYPE("(iiay)"), NULL, 0);

    gint width = gdk_texture_get_width(texture);
    gint height = gdk_texture_get_height(texture);
    gsize stride = (gsize)width * 4;
    guchar *pixels = g_malloc(stride * (gsize)height);
    gdk_texture_download(texture, pixels, stride);

    GVariantBuilder array;
    g_variant_builder_init(&array, G_VARIANT_TYPE("a(iiay)"));
    GVariantBuilder bytes;
    g_variant_builder_init(&bytes, G_VARIANT_TYPE("ay"));
    for (gint y = 0; y < height; y++) {
        for (gint x = 0; x < width; x++) {
            const guchar *p = pixels + y * stride + x * 4;
            guint32 argb = ((guint32)p[3] << 24) | ((guint32)p[2] << 16) |
                           ((guint32)p[1] << 8) | (guint32)p[0];
            g_variant_builder_add(&bytes, "y", (guchar)(argb >> 24));
            g_variant_builder_add(&bytes, "y", (guchar)(argb >> 16));
            g_variant_builder_add(&bytes, "y", (guchar)(argb >> 8));
            g_variant_builder_add(&bytes, "y", (guchar)argb);
        }
    }
    g_variant_builder_add(&array, "(ii@ay)", width, height, g_variant_builder_end(&bytes));
    g_free(pixels);
    g_object_unref(texture);
    return g_variant_builder_end(&array);
}

static GVariant *empty_pixmap(void)
{
    return g_variant_new_array(G_VARIANT_TYPE("(iiay)"), NULL, 0);
}

static GVariant *tooltip(Y720Tray *tray)
{
    gchar *subtitle = g_strdup_printf("%s · %d RPM", mode_label(tray->mode), tray->rpm);
    GVariant *value = g_variant_new("(s@a(iiay)ss)", "Y720 Nerve Center", empty_pixmap(),
                                    subtitle, tray->window_visible ? "窗口已显示" : "窗口已隐藏");
    g_free(subtitle);
    return value;
}

static GVariant *sni_property(Y720Tray *tray, const gchar *name)
{
    if (g_strcmp0(name, "Category") == 0)
        return g_variant_new_string("ApplicationStatus");
    if (g_strcmp0(name, "Id") == 0)
        return g_variant_new_string("y720-control");
    if (g_strcmp0(name, "Title") == 0)
        return g_variant_new_string("Y720 Nerve Center");
    if (g_strcmp0(name, "Status") == 0)
        return g_variant_new_string("Active");
    if (g_strcmp0(name, "IconName") == 0)
        return g_variant_new_string("");
    if (g_strcmp0(name, "IconPixmap") == 0)
        return icon_pixmap(tray);
    if (g_strcmp0(name, "OverlayIconName") == 0 ||
        g_strcmp0(name, "AttentionIconName") == 0 ||
        g_strcmp0(name, "AttentionMovieName") == 0)
        return g_variant_new_string("");
    if (g_strcmp0(name, "OverlayIconPixmap") == 0 ||
        g_strcmp0(name, "AttentionIconPixmap") == 0)
        return empty_pixmap();
    if (g_strcmp0(name, "ToolTip") == 0)
        return tooltip(tray);
    if (g_strcmp0(name, "ItemIsMenu") == 0)
        return g_variant_new_boolean(FALSE);
    if (g_strcmp0(name, "Menu") == 0)
        return g_variant_new_object_path("/MenuBar");
    if (g_strcmp0(name, "WindowId") == 0)
        return g_variant_new_int32(0);
    return NULL;
}

static GVariant *sni_get_property(GDBusConnection *connection, const gchar *sender,
                                  const gchar *object_path, const gchar *interface_name,
                                  const gchar *property_name, GError **error,
                                  gpointer user_data)
{
    (void)connection; (void)sender; (void)object_path; (void)interface_name;
    Y720Tray *tray = user_data;
    GVariant *value = sni_property(tray, property_name);
    if (!value) {
        g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                    "Unknown property %s", property_name);
    }
    return value;
}

static void sni_method_call(GDBusConnection *connection, const gchar *sender,
                            const gchar *object_path, const gchar *interface_name,
                            const gchar *method_name, GVariant *parameters,
                            GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)connection; (void)sender; (void)object_path; (void)interface_name;
    (void)parameters;
    Y720Tray *tray = user_data;
    if (g_strcmp0(method_name, "Activate") == 0 ||
        g_strcmp0(method_name, "SecondaryActivate") == 0) {
        if (tray->callback)
            tray->callback("toggle-window", tray->user_data);
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static const GDBusInterfaceVTable sni_vtable = {
    .method_call = sni_method_call,
    .get_property = sni_get_property,
};

static GVariant *menu_item(Y720Tray *tray, gint id, GVariant *children)
{
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));

    if (id == ITEM_ROOT || id == ITEM_PROFILES) {
        if (id == ITEM_PROFILES) {
            add_prop(&props, "label", g_variant_new_string("切换方案"));
            add_prop(&props, "enabled", g_variant_new_boolean(TRUE));
            add_prop(&props, "visible", g_variant_new_boolean(TRUE));
        }
        add_prop(&props, "children-display", g_variant_new_string("submenu"));
    } else if (id >= ITEM_PROFILE_BASE) {
        guint index = (guint)(id - ITEM_PROFILE_BASE);
        if (index < tray->profiles->len) {
            add_prop(&props, "label", g_variant_new_string(g_ptr_array_index(tray->profiles, index)));
            add_prop(&props, "enabled", g_variant_new_boolean(TRUE));
            add_prop(&props, "visible", g_variant_new_boolean(TRUE));
            add_prop(&props, "toggle-type", g_variant_new_string("radio"));
            add_prop(&props, "toggle-state",
                     g_variant_new_int32(index == tray->profile_selected ? 1 : 0));
        }
    } else if (id == ITEM_SEP1 || id == ITEM_SEP2 || id == ITEM_SEP3) {
        add_prop(&props, "type", g_variant_new_string("separator"));
    } else if (id == ITEM_TOGGLE) {
        add_prop(&props, "label", g_variant_new_string(tray->window_visible ? "隐藏窗口" : "显示窗口"));
        add_prop(&props, "enabled", g_variant_new_boolean(TRUE));
        add_prop(&props, "visible", g_variant_new_boolean(TRUE));
    } else if (id == ITEM_AUTO || id == ITEM_BOOST || id == ITEM_CURVE) {
        gboolean selected = id == ITEM_AUTO ? mode_is(tray, "auto") :
                            id == ITEM_BOOST ? mode_is(tray, "boost") : mode_is(tray, "curve");
        add_prop(&props, "label", g_variant_new_string(id == ITEM_AUTO ? "自动模式" :
                                                       id == ITEM_BOOST ? "极致散热" : "曲线模式"));
        gboolean curve_hidden = id == ITEM_CURVE && !tray->curve_enabled;
        add_prop(&props, "enabled", g_variant_new_boolean(tray->controls && !curve_hidden));
        add_prop(&props, "visible", g_variant_new_boolean(!curve_hidden));
        add_prop(&props, "toggle-type", g_variant_new_string("radio"));
        add_prop(&props, "toggle-state", g_variant_new_int32(selected ? 1 : 0));
    } else if (id == ITEM_SETTINGS) {
        add_prop(&props, "label", g_variant_new_string("设置…"));
        add_prop(&props, "enabled", g_variant_new_boolean(TRUE));
        add_prop(&props, "visible", g_variant_new_boolean(TRUE));
    } else if (id == ITEM_QUIT) {
        add_prop(&props, "label", g_variant_new_string("退出"));
        add_prop(&props, "enabled", g_variant_new_boolean(TRUE));
        add_prop(&props, "visible", g_variant_new_boolean(TRUE));
    }

    GVariantBuilder item;
    g_variant_builder_init(&item, G_VARIANT_TYPE("(ia{sv}av)"));
    g_variant_builder_add(&item, "i", id);
    g_variant_builder_add_value(&item, g_variant_builder_end(&props));
    g_variant_builder_add_value(&item, children);
    return g_variant_builder_end(&item);
}

static GVariant *build_menu(Y720Tray *tray)
{
    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));

    const gint ids[] = {ITEM_TOGGLE, ITEM_SEP1, ITEM_AUTO, ITEM_BOOST, ITEM_CURVE,
                        ITEM_SEP2, ITEM_PROFILES, ITEM_SEP3, ITEM_SETTINGS, ITEM_QUIT};
    for (guint i = 0; i < G_N_ELEMENTS(ids); i++) {
        if (ids[i] == ITEM_PROFILES) {
            GVariantBuilder profile_children;
            g_variant_builder_init(&profile_children, G_VARIANT_TYPE("av"));
            for (guint p = 0; p < tray->profiles->len; p++) {
                GVariantBuilder empty;
                g_variant_builder_init(&empty, G_VARIANT_TYPE("av"));
                GVariant *child = menu_item(tray, (gint)(ITEM_PROFILE_BASE + p),
                                            g_variant_builder_end(&empty));
                g_variant_builder_add_value(&profile_children, g_variant_new_variant(child));
            }
            GVariant *profiles = menu_item(tray, ITEM_PROFILES,
                                           g_variant_builder_end(&profile_children));
            g_variant_builder_add_value(&children, g_variant_new_variant(profiles));
        } else {
            GVariantBuilder empty;
            g_variant_builder_init(&empty, G_VARIANT_TYPE("av"));
            GVariant *child = menu_item(tray, ids[i], g_variant_builder_end(&empty));
            g_variant_builder_add_value(&children, g_variant_new_variant(child));
        }
    }
    return menu_item(tray, ITEM_ROOT, g_variant_builder_end(&children));
}

static GVariant *menu_property(Y720Tray *tray, const gchar *name)
{
    (void)tray;
    if (g_strcmp0(name, "Version") == 0)
        return g_variant_new_uint32(3);
    if (g_strcmp0(name, "TextDirection") == 0)
        return g_variant_new_string("ltr");
    if (g_strcmp0(name, "Status") == 0)
        return g_variant_new_string("normal");
    if (g_strcmp0(name, "IconThemePath") == 0)
        return g_variant_new_strv(NULL, 0);
    return NULL;
}

static GVariant *menu_get_property(GDBusConnection *connection, const gchar *sender,
                                   const gchar *object_path, const gchar *interface_name,
                                   const gchar *property_name, GError **error,
                                   gpointer user_data)
{
    (void)connection; (void)sender; (void)object_path; (void)interface_name;
    GVariant *value = menu_property(user_data, property_name);
    if (!value)
        g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                    "Unknown property %s", property_name);
    return value;
}

static GVariant *item_properties(Y720Tray *tray, gint id)
{
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    if (id == ITEM_ROOT || id == ITEM_PROFILES) {
        if (id == ITEM_PROFILES) {
            add_prop(&props, "label", g_variant_new_string("切换方案"));
            add_prop(&props, "enabled", g_variant_new_boolean(TRUE));
            add_prop(&props, "visible", g_variant_new_boolean(TRUE));
        }
        add_prop(&props, "children-display", g_variant_new_string("submenu"));
    } else if (id >= ITEM_PROFILE_BASE) {
        guint index = (guint)(id - ITEM_PROFILE_BASE);
        if (index < tray->profiles->len) {
            add_prop(&props, "label", g_variant_new_string(g_ptr_array_index(tray->profiles, index)));
            add_prop(&props, "enabled", g_variant_new_boolean(TRUE));
            add_prop(&props, "visible", g_variant_new_boolean(TRUE));
            add_prop(&props, "toggle-type", g_variant_new_string("radio"));
            add_prop(&props, "toggle-state",
                     g_variant_new_int32(index == tray->profile_selected ? 1 : 0));
        }
    } else if (id == ITEM_SEP1 || id == ITEM_SEP2 || id == ITEM_SEP3) {
        add_prop(&props, "type", g_variant_new_string("separator"));
    } else if (id == ITEM_TOGGLE) {
        add_prop(&props, "label", g_variant_new_string(tray->window_visible ? "隐藏窗口" : "显示窗口"));
        add_prop(&props, "enabled", g_variant_new_boolean(TRUE));
        add_prop(&props, "visible", g_variant_new_boolean(TRUE));
    } else if (id == ITEM_AUTO || id == ITEM_BOOST || id == ITEM_CURVE) {
        gboolean selected = id == ITEM_AUTO ? mode_is(tray, "auto") :
                            id == ITEM_BOOST ? mode_is(tray, "boost") : mode_is(tray, "curve");
        gboolean curve_hidden = id == ITEM_CURVE && !tray->curve_enabled;
        add_prop(&props, "label", g_variant_new_string(id == ITEM_AUTO ? "自动模式" :
                                                       id == ITEM_BOOST ? "极致散热" : "曲线模式"));
        add_prop(&props, "enabled", g_variant_new_boolean(tray->controls && !curve_hidden));
        add_prop(&props, "visible", g_variant_new_boolean(!curve_hidden));
        add_prop(&props, "toggle-type", g_variant_new_string("radio"));
        add_prop(&props, "toggle-state", g_variant_new_int32(selected ? 1 : 0));
    } else if (id == ITEM_SETTINGS) {
        add_prop(&props, "label", g_variant_new_string("设置…"));
        add_prop(&props, "enabled", g_variant_new_boolean(TRUE));
        add_prop(&props, "visible", g_variant_new_boolean(TRUE));
    } else if (id == ITEM_QUIT) {
        add_prop(&props, "label", g_variant_new_string("退出"));
        add_prop(&props, "enabled", g_variant_new_boolean(TRUE));
        add_prop(&props, "visible", g_variant_new_boolean(TRUE));
    }
    return g_variant_builder_end(&props);
}

static void menu_method_call(GDBusConnection *connection, const gchar *sender,
                             const gchar *object_path, const gchar *interface_name,
                             const gchar *method_name, GVariant *parameters,
                             GDBusMethodInvocation *invocation, gpointer user_data)
{
    (void)connection; (void)sender; (void)object_path; (void)interface_name;
    Y720Tray *tray = user_data;

    if (g_strcmp0(method_name, "GetLayout") == 0) {
        GVariant *root = build_menu(tray);
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(u@(ia{sv}av))", tray->revision, root));
        return;
    }

    if (g_strcmp0(method_name, "GetGroupProperties") == 0) {
        GVariant *ids_value = g_variant_get_child_value(parameters, 0);
        gsize count = 0;
        const gint32 *ids = g_variant_get_fixed_array(ids_value, &count, sizeof(gint32));
        GVariantBuilder result;
        g_variant_builder_init(&result, G_VARIANT_TYPE("a(ia{sv})"));
        if (count == 0) {
            GArray *all = g_array_new(FALSE, FALSE, sizeof(gint));
            const gint base[] = {ITEM_ROOT, ITEM_TOGGLE, ITEM_SEP1, ITEM_AUTO,
                                 ITEM_BOOST, ITEM_CURVE, ITEM_SEP2, ITEM_PROFILES,
                                 ITEM_SEP3, ITEM_SETTINGS, ITEM_QUIT};
            for (guint i = 0; i < G_N_ELEMENTS(base); i++)
                g_array_append_val(all, base[i]);
            for (guint i = 0; i < tray->profiles->len; i++) {
                gint id = ITEM_PROFILE_BASE + (gint)i;
                g_array_append_val(all, id);
            }
            for (guint i = 0; i < all->len; i++) {
                gint id = g_array_index(all, gint, i);
                g_variant_builder_add(&result, "(i@a{sv})", id, item_properties(tray, id));
            }
            g_array_free(all, TRUE);
        } else {
            for (gsize i = 0; i < count; i++)
                g_variant_builder_add(&result, "(i@a{sv})", ids[i], item_properties(tray, ids[i]));
        }
        g_variant_unref(ids_value);
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&result)));
        return;
    }

    if (g_strcmp0(method_name, "GetProperty") == 0) {
        gint32 id = g_variant_get_int32(g_variant_get_child_value(parameters, 0));
        const gchar *name = NULL;
        g_variant_get_child(parameters, 1, "&s", &name);
        GVariant *props = item_properties(tray, id);
        GVariant *value = g_variant_lookup_value(props, name, NULL);
        g_variant_unref(props);
        if (!value)
            value = g_variant_new_string("");
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
        return;
    }

    if (g_strcmp0(method_name, "Event") == 0) {
        gint32 id = g_variant_get_int32(g_variant_get_child_value(parameters, 0));
        const gchar *event = NULL;
        g_variant_get_child(parameters, 1, "&s", &event);
        if (g_strcmp0(event, "clicked") == 0) {
            if (id >= ITEM_PROFILE_BASE) {
                gchar action_name[32];
                g_snprintf(action_name, sizeof(action_name), "profile:%d",
                           id - ITEM_PROFILE_BASE);
                if (tray->callback)
                    tray->callback(action_name, tray->user_data);
                g_dbus_method_invocation_return_value(invocation, NULL);
                return;
            }
            const gchar *action = NULL;
            switch (id) {
            case ITEM_TOGGLE: action = "toggle-window"; break;
            case ITEM_AUTO: action = "mode-auto"; break;
            case ITEM_BOOST: action = "mode-boost"; break;
            case ITEM_CURVE: action = "mode-curve"; break;
            case ITEM_SETTINGS: action = "settings"; break;
            case ITEM_QUIT: action = "quit"; break;
            default: break;
            }
            if (action && tray->callback)
                tray->callback(action, tray->user_data);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (g_strcmp0(method_name, "EventGroup") == 0) {
        GVariantBuilder errors;
        g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(@ai)", g_variant_builder_end(&errors)));
        return;
    }

    if (g_strcmp0(method_name, "AboutToShow") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
        return;
    }

    if (g_strcmp0(method_name, "AboutToShowGroup") == 0) {
        GVariant *empty1 = g_variant_new_array(G_VARIANT_TYPE_INT32, NULL, 0);
        GVariant *empty2 = g_variant_new_array(G_VARIANT_TYPE_INT32, NULL, 0);
        g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(@ai@ai)", empty1, empty2));
        return;
    }

    g_dbus_method_invocation_return_dbus_error(invocation,
        "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method");
}

static const GDBusInterfaceVTable menu_vtable = {
    .method_call = menu_method_call,
    .get_property = menu_get_property,
};

static void on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    (void)connection;
    Y720Tray *tray = user_data;
    tray->name_owned = TRUE;
    g_dbus_connection_call(tray->bus,
        "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
        g_variant_new("(s)", name), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    /* Push the populated menu before the first panel request can be cached. */
    emit_layout_updated(tray);
    g_dbus_connection_emit_signal(tray->bus, NULL, "/StatusNotifierItem",
                                  "org.kde.StatusNotifierItem", "NewToolTip", NULL, NULL);
}

static void on_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    (void)connection; (void)name;
    Y720Tray *tray = user_data;
    tray->name_owned = FALSE;
}

Y720Tray *y720_tray_new(GDBusConnection *bus, Y720TrayActionFunc callback, gpointer user_data)
{
    Y720Tray *tray = g_new0(Y720Tray, 1);
    tray->bus = g_object_ref(bus);
    tray->callback = callback;
    tray->user_data = user_data;
    tray->mode = g_strdup("auto");
    tray->profiles = g_ptr_array_new_with_free_func(g_free);
    tray->object_path = g_strdup("/StatusNotifierItem");

    GVariant *reply = g_dbus_connection_call_sync(bus,
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameHasOwner",
        g_variant_new("(s)", "org.kde.StatusNotifierWatcher"),
        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    if (reply) {
        GVariant *child = g_variant_get_child_value(reply, 0);
        tray->watcher_present = g_variant_get_boolean(child);
        g_variant_unref(child);
        g_variant_unref(reply);
    }

    if (!tray->watcher_present) {
        g_warning("StatusNotifierWatcher is not available; tray icon disabled");
        return tray;
    }

    GError *error = NULL;
    GDBusNodeInfo *sni = g_dbus_node_info_new_for_xml(sni_xml, &error);
    GDBusNodeInfo *menu = g_dbus_node_info_new_for_xml(menu_xml, &error);
    if (!sni || !menu) {
        g_warning("tray interface XML failed: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        if (sni) g_dbus_node_info_unref(sni);
        if (menu) g_dbus_node_info_unref(menu);
        return tray;
    }

    tray->sni_registration = g_dbus_connection_register_object(bus, "/StatusNotifierItem",
        sni->interfaces[0], &sni_vtable, tray, NULL, &error);
    tray->menu_registration = g_dbus_connection_register_object(bus, "/MenuBar",
        menu->interfaces[0], &menu_vtable, tray, NULL, &error);
    g_dbus_node_info_unref(sni);
    g_dbus_node_info_unref(menu);

    if (!tray->sni_registration || !tray->menu_registration) {
        g_warning("tray object registration failed: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        return tray;
    }

    tray->service_name = g_strdup_printf("org.kde.StatusNotifierItem-%d-1", (gint)getpid());
    tray->owner_id = g_bus_own_name_on_connection(bus, tray->service_name, G_BUS_NAME_OWNER_FLAGS_NONE,
                                                  on_name_acquired, on_name_lost, tray, NULL);
    return tray;
}

void y720_tray_set_state(Y720Tray *tray, gboolean window_visible, gboolean controls_available,
                         const gchar *mode, gint rpm)
{
    if (!tray)
        return;
    gboolean extreme = controls_available && g_strcmp0(mode, "boost") == 0;
    gboolean icon_changed = tray->extreme != extreme;
    gboolean changed = tray->window_visible != window_visible ||
                       tray->controls != controls_available ||
                       g_strcmp0(tray->mode, mode) != 0 || tray->rpm != rpm;
    tray->window_visible = window_visible;
    tray->controls = controls_available;
    tray->extreme = extreme;
    g_free(tray->mode);
    tray->mode = g_strdup(mode ? mode : "auto");
    tray->rpm = rpm;
    if (changed && tray->name_owned)
        emit_layout_updated(tray);
    if (icon_changed && tray->name_owned)
        g_dbus_connection_emit_signal(tray->bus, NULL, "/StatusNotifierItem",
                                      "org.kde.StatusNotifierItem", "NewIcon", NULL, NULL);
}

void y720_tray_set_hotkey_state(Y720Tray *tray, gboolean ok)
{
    tray->hotkey_ok = ok;
}

gboolean y720_tray_is_available(Y720Tray *tray)
{
    return tray && tray->watcher_present;
}

void y720_tray_free(Y720Tray *tray)
{
    if (!tray)
        return;
    if (tray->owner_id)
        g_bus_unown_name(tray->owner_id);
    g_free(tray->service_name);
    if (tray->sni_registration)
        g_dbus_connection_unregister_object(tray->bus, tray->sni_registration);
    if (tray->menu_registration)
        g_dbus_connection_unregister_object(tray->bus, tray->menu_registration);
    g_clear_object(&tray->bus);
    g_free(tray->object_path);
    g_free(tray->mode);
    g_ptr_array_free(tray->profiles, TRUE);
    g_free(tray);
}

void y720_tray_set_profiles(Y720Tray *tray, const gchar *const *names, guint count,
                            guint selected)
{
    if (!tray)
        return;
    g_ptr_array_set_size(tray->profiles, 0);
    for (guint i = 0; i < count; i++)
        g_ptr_array_add(tray->profiles, g_strdup(names[i] ? names[i] : ""));
    tray->profile_selected = selected;
    if (tray->name_owned)
        emit_layout_updated(tray);
}

void y720_tray_set_curve_enabled(Y720Tray *tray, gboolean enabled)
{
    if (!tray || tray->curve_enabled == enabled)
        return;
    tray->curve_enabled = enabled;
    if (tray->name_owned)
        emit_layout_updated(tray);
}
