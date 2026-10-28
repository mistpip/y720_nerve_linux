/* SPDX-License-Identifier: MIT */
#include "y720-hotkeys.h"

#include <gdk/gdk.h>
#include <gdk/x11/gdkx.h>
#include <gtk/gtk.h>

#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xproto.h>

#define MAX_BINDINGS 4

typedef struct {
    KeyCode keycode;
    guint mods;
    gboolean pressed;
    Y720HotkeyFunc callback;
    gpointer user_data;
} Binding;

static Display *x_display;
static Window x_root;
static GIOChannel *x_channel;
static guint x_watch;
static Binding x_bindings[MAX_BINDINGS];
static guint x_count;
static gboolean x_grab_failed;

static int grab_error_handler(Display *display, XErrorEvent *event)
{
    (void)display;
    if (event->error_code == BadAccess)
        x_grab_failed = TRUE;
    return 0;
}

static gboolean x_event_cb(GIOChannel *source, GIOCondition condition, gpointer user_data)
{
    (void)source; (void)condition; (void)user_data;
    while (XPending(x_display)) {
        XEvent event;
        XNextEvent(x_display, &event);
        if (event.type != KeyPress && event.type != KeyRelease)
            continue;
        guint state = event.xkey.state & ~(LockMask | Mod2Mask);
        for (guint i = 0; i < x_count; i++) {
            Binding *binding = &x_bindings[i];
            if (event.xkey.keycode != binding->keycode || state != binding->mods)
                continue;
            if (event.type == KeyPress && !binding->pressed) {
                binding->pressed = TRUE;
                if (binding->callback)
                    binding->callback(binding->user_data);
            } else if (event.type == KeyRelease) {
                binding->pressed = FALSE;
            }
            break;
        }
    }
    return G_SOURCE_CONTINUE;
}

static void ungrab_binding(Display *display, Window root, KeyCode keycode, guint mods)
{
    const guint locks[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
    for (guint i = 0; i < G_N_ELEMENTS(locks); i++)
        XUngrabKey(display, keycode, mods | locks[i], root);
}
#endif

gboolean y720_hotkeys_start(const Y720HotkeyBinding *bindings, guint count,
                            GError **error)
{
#ifdef HAVE_X11
    GdkDisplay *display = gdk_display_get_default();
    if (!display || !GDK_IS_X11_DISPLAY(display)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "全局快捷键仅在 X11 会话可用");
        return FALSE;
    }
    if (!bindings || count == 0 || count > MAX_BINDINGS) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "快捷键数量无效");
        return FALSE;
    }

    x_display = XOpenDisplay(NULL);
    if (!x_display) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "无法打开 X11 显示");
        return FALSE;
    }

    int opcode = 0, event_base = 0, error_base = 0;
    if (XQueryExtension(x_display, "XWAYLAND", &opcode, &event_base, &error_base)) {
        XCloseDisplay(x_display);
        x_display = NULL;
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "XWayland 会话无法提供全局快捷键");
        return FALSE;
    }

    x_root = DefaultRootWindow(x_display);
    x_count = 0;
    x_grab_failed = FALSE;
    int (*old_handler)(Display *, XErrorEvent *) = XSetErrorHandler(grab_error_handler);
    const guint locks[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};

    for (guint i = 0; i < count; i++) {
        guint keyval = 0;
        GdkModifierType gdk_mods = 0;
        gtk_accelerator_parse(bindings[i].accelerator, &keyval, &gdk_mods);
        if (!keyval || !bindings[i].callback) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "无法解析快捷键");
            goto fail;
        }
        KeyCode keycode = XKeysymToKeycode(x_display, keyval);
        guint mods = 0;
        if (gdk_mods & GDK_CONTROL_MASK) mods |= ControlMask;
        if (gdk_mods & GDK_SHIFT_MASK) mods |= ShiftMask;
        if (gdk_mods & GDK_ALT_MASK) mods |= Mod1Mask;
        if (gdk_mods & GDK_SUPER_MASK) mods |= Mod4Mask;
        if (!keycode || !mods) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "快捷键需要修饰键");
            goto fail;
        }
        for (guint j = 0; j < x_count; j++) {
            if (x_bindings[j].keycode == keycode && x_bindings[j].mods == mods) {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "两个快捷键重复");
                goto fail;
            }
        }
        Binding *binding = &x_bindings[x_count++];
        binding->keycode = keycode;
        binding->mods = mods;
        binding->pressed = FALSE;
        binding->callback = bindings[i].callback;
        binding->user_data = bindings[i].user_data;
        for (guint k = 0; k < G_N_ELEMENTS(locks); k++)
            XGrabKey(x_display, keycode, mods | locks[k], x_root, False,
                     GrabModeAsync, GrabModeAsync);
    }

    XSync(x_display, False);
    XSetErrorHandler(old_handler);

    if (x_grab_failed) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_BUSY, "快捷键已被其他程序占用");
        goto fail;
    }

    x_channel = g_io_channel_unix_new(ConnectionNumber(x_display));
    g_io_channel_set_encoding(x_channel, NULL, NULL);
    g_io_channel_set_buffered(x_channel, FALSE);
    x_watch = g_io_add_watch(x_channel, G_IO_IN, x_event_cb, NULL);
    return TRUE;

fail:
    XSetErrorHandler(old_handler);
    XSync(x_display, False);
    for (guint i = 0; i < x_count; i++)
        ungrab_binding(x_display, x_root, x_bindings[i].keycode, x_bindings[i].mods);
    x_count = 0;
    XCloseDisplay(x_display);
    x_display = NULL;
    return FALSE;
#else
    (void)bindings; (void)count;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "构建时未启用 X11");
    return FALSE;
#endif
}

void y720_hotkeys_stop(void)
{
#ifdef HAVE_X11
    if (x_watch) {
        g_source_remove(x_watch);
        x_watch = 0;
    }
    if (x_channel) {
        g_io_channel_unref(x_channel);
        x_channel = NULL;
    }
    if (x_display) {
        for (guint i = 0; i < x_count; i++)
            ungrab_binding(x_display, x_root, x_bindings[i].keycode, x_bindings[i].mods);
        XSync(x_display, False);
        XCloseDisplay(x_display);
        x_display = NULL;
    }
    x_count = 0;
#endif
}

gboolean y720_hotkeys_active(void)
{
#ifdef HAVE_X11
    return x_display != NULL;
#else
    return FALSE;
#endif
}
