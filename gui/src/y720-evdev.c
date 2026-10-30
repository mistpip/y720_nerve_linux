/* SPDX-License-Identifier: MIT */
#include "y720-evdev.h"
#include "y720-i18n.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct {
    gint code;
    Y720HotkeyFunc callback;
    gpointer user_data;
    gboolean pressed;
} EvdevKey;

static EvdevKey keys[4];
static guint key_count;
static int ev_fd = -1;
static GIOChannel *ev_channel;
static guint ev_watch;
static guint reconnect_timer;
static gboolean ev_suspended;
static gboolean ev_dropped;

gboolean y720_evdev_active(void) { return ev_fd >= 0 && !ev_suspended; }

static gboolean target_id_ok(int fd)
{
    struct input_id id;
    return ioctl(fd, EVIOCGID, &id) == 0 && id.bustype == BUS_USB &&
           id.vendor == 0x048d && id.product == 0xc100 && id.version == 0x0110;
}

static void reset_pressed(void)
{
    for (guint i = 0; i < key_count; i++)
        keys[i].pressed = FALSE;
}

static gboolean is_target_keyboard(const gchar *path)
{
    const gchar *fields[] = {"bustype", "vendor", "product", "version"};
    const guint expected[] = {BUS_USB, 0x048d, 0xc100, 0x0110};
    for (guint i = 0; i < G_N_ELEMENTS(fields); i++) {
        gchar *file = g_build_filename(path, "device", "id", fields[i], NULL);
        gchar *value = NULL;
        gboolean ok = g_file_get_contents(file, &value, NULL, NULL) &&
                      g_ascii_strtoull(value, NULL, 16) == expected[i];
        g_free(file);
        g_free(value);
        if (!ok)
            return FALSE;
    }
    gchar *name_path = g_build_filename(path, "device", "name", NULL);
    gchar *name = NULL;
    gboolean ok = g_file_get_contents(name_path, &name, NULL, NULL) &&
                  g_strcmp0(g_strstrip(name), "ITE Tech. Inc. ITE Device(8910) Keyboard") == 0;
    g_free(name_path);
    g_free(name);
    return ok;
}

static gchar *find_target_device(void)
{
    DIR *dir = opendir("/sys/class/input");
    if (!dir)
        return NULL;
    gchar *device = NULL;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!g_str_has_prefix(entry->d_name, "event"))
            continue;
        gchar *sys = g_build_filename("/sys/class/input", entry->d_name, NULL);
        if (is_target_keyboard(sys)) {
            device = g_strdup_printf("/dev/input/%s", entry->d_name);
            g_free(sys);
            break;
        }
        g_free(sys);
    }
    closedir(dir);
    return device;
}

static void close_events(void)
{
    if (ev_watch) {
        g_source_remove(ev_watch);
        ev_watch = 0;
    }
    g_clear_pointer(&ev_channel, g_io_channel_unref);
    if (ev_fd >= 0) {
        close(ev_fd);
        ev_fd = -1;
    }
    reset_pressed();
    ev_dropped = FALSE;
}

static gboolean ev_event(GIOChannel *source, GIOCondition condition, gpointer user_data)
{
    (void)source; (void)user_data;
    if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        ev_watch = 0;
        close_events();
        return G_SOURCE_REMOVE;
    }
    struct input_event events[16];
    ssize_t n;
    while ((n = read(ev_fd, events, sizeof(events))) > 0) {
        gsize count = (gsize)n / sizeof(events[0]);
        for (gsize i = 0; i < count; i++) {
            if (events[i].type == EV_SYN && events[i].code == SYN_DROPPED) {
                ev_dropped = TRUE;
                reset_pressed();
                continue;
            }
            if (ev_dropped) {
                if (events[i].type == EV_SYN && events[i].code == SYN_REPORT)
                    ev_dropped = FALSE;
                continue;
            }
            if (ev_suspended || events[i].type != EV_KEY)
                continue;
            for (guint k = 0; k < key_count; k++) {
                if (events[i].code != keys[k].code)
                    continue;
                if (events[i].value == 1 && !keys[k].pressed) {
                    keys[k].pressed = TRUE;
                    if (keys[k].callback)
                        keys[k].callback(keys[k].user_data);
                } else if (events[i].value == 0) {
                    keys[k].pressed = FALSE;
                }
            }
        }
    }
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        ev_watch = 0;
        close_events();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean connect_events(GError **error)
{
    gchar *device = find_target_device();
    if (!device) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, y720_tr("没有找到 Y720 键盘输入节点"));
        return FALSE;
    }
    ev_fd = open(device, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (ev_fd < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), y720_tr("无法读取 %s：%s"), device, g_strerror(errno));
        g_free(device);
        return FALSE;
    }
    g_free(device);
    if (!target_id_ok(ev_fd)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, y720_tr("键盘身份已改变"));
        close_events();
        return FALSE;
    }
    ev_channel = g_io_channel_unix_new(ev_fd);
    g_io_channel_set_encoding(ev_channel, NULL, NULL);
    g_io_channel_set_buffered(ev_channel, FALSE);
    ev_watch = g_io_add_watch(ev_channel, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, ev_event, NULL);
    return TRUE;
}

static gboolean reconnect_events(gpointer unused)
{
    (void)unused;
    if (ev_fd < 0 && key_count)
        connect_events(NULL);
    return G_SOURCE_CONTINUE;
}

gboolean y720_evdev_start(const Y720HotkeyBinding *bindings, guint count, GError **error)
{
    y720_evdev_stop();
    for (guint i = 0; i < count && key_count < G_N_ELEMENTS(keys); i++) {
        if (!bindings[i].accelerator || !g_str_has_prefix(bindings[i].accelerator, "evdev:"))
            continue;
        gchar *end = NULL;
        guint64 code = g_ascii_strtoull(bindings[i].accelerator + 6, &end, 10);
        if (code == 0 || code > KEY_MAX || !end || *end) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, y720_tr("evdev 键码无效"));
            return FALSE;
        }
        gboolean duplicate = FALSE;
        for (guint j = 0; j < key_count; j++)
            duplicate |= keys[j].code == (gint)code;
        if (duplicate)
            continue;
        keys[key_count].code = (gint)code;
        keys[key_count].callback = bindings[i].callback;
        keys[key_count].user_data = bindings[i].user_data;
        keys[key_count].pressed = FALSE;
        key_count++;
    }
    if (key_count == 0)
        return TRUE;

    reconnect_timer = g_timeout_add_seconds(3, reconnect_events, NULL);
    return connect_events(error);
}

void y720_evdev_stop(void)
{
    if (reconnect_timer) {
        g_source_remove(reconnect_timer);
        reconnect_timer = 0;
    }
    close_events();
    key_count = 0;
}

typedef struct {
    Y720EvdevCaptureFunc callback;
    gpointer user_data;
} CaptureState;

static int cap_fd = -1;
static GIOChannel *cap_channel;
static guint cap_watch;
static CaptureState cap_state;

static gboolean capture_event(GIOChannel *source, GIOCondition condition, gpointer user_data)
{
    (void)source; (void)user_data;
    if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        cap_watch = 0;
        y720_evdev_capture_stop();
        return G_SOURCE_REMOVE;
    }
    struct input_event events[16];
    ssize_t n;
    while ((n = read(cap_fd, events, sizeof(events))) > 0) {
        gsize count = (gsize)n / sizeof(events[0]);
        for (gsize i = 0; i < count; i++) {
            if (events[i].type == EV_KEY && events[i].value == 1 && cap_state.callback) {
                Y720EvdevCaptureFunc callback = cap_state.callback;
                /* Modifier presses do not finish capture. The client may ignore
                 * them, or stop capture while accepting a later special key. */
                callback(events[i].code, cap_state.user_data);
                if (cap_fd < 0 || cap_state.callback != callback)
                    return G_SOURCE_REMOVE;
            }
        }
    }
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        cap_watch = 0;
        y720_evdev_capture_stop();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

gboolean y720_evdev_capture_start(Y720EvdevCaptureFunc callback, gpointer user_data,
                                  GError **error)
{
    y720_evdev_capture_stop();
    gchar *device = find_target_device();
    if (!device) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, y720_tr("没有找到 Y720 键盘输入节点"));
        return FALSE;
    }
    cap_fd = open(device, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (cap_fd < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    y720_tr("没有读取 %s 的权限"), device);
        g_free(device);
        return FALSE;
    }
    g_free(device);
    if (!target_id_ok(cap_fd)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, y720_tr("键盘身份已改变"));
        y720_evdev_capture_stop();
        return FALSE;
    }
    cap_state.callback = callback;
    cap_state.user_data = user_data;
    ev_suspended = TRUE;
    cap_channel = g_io_channel_unix_new(cap_fd);
    g_io_channel_set_encoding(cap_channel, NULL, NULL);
    g_io_channel_set_buffered(cap_channel, FALSE);
    cap_watch = g_io_add_watch(cap_channel, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, capture_event, NULL);
    return TRUE;
}

void y720_evdev_capture_stop(void)
{
    if (cap_watch) {
        g_source_remove(cap_watch);
        cap_watch = 0;
    }
    if (cap_channel) {
        g_io_channel_unref(cap_channel);
        cap_channel = NULL;
    }
    if (cap_fd >= 0) {
        close(cap_fd);
        cap_fd = -1;
    }
    cap_state.callback = NULL;
    cap_state.user_data = NULL;
    ev_suspended = FALSE;
    reset_pressed();
}
