/* SPDX-License-Identifier: MIT */
#include "y720-settings.h"
#include "y720-i18n.h"

#include <errno.h>
#include <stdio.h>
#include <gtk/gtk.h>
#include <string.h>
#include <unistd.h>

static gboolean valid_accel(const gchar *accel)
{
    if (!accel || !*accel)
        return FALSE;
    if (g_str_has_prefix(accel, "keycode:")) {
        gchar *end;
        guint64 code = g_ascii_strtoull(accel + 8, &end, 10);
        return *end == '\0' && code > 0 && code <= 255;
    }
    if (g_str_has_prefix(accel, "evdev:")) {
        gchar *end;
        guint64 code = g_ascii_strtoull(accel + 6, &end, 10);
        return *end == '\0' && code > 0 && code <= 0x2ff;
    }
    guint keyval = 0;
    GdkModifierType mods = 0;
    gtk_accelerator_parse(accel, &keyval, &mods);
    return keyval != 0;
}

static gchar *config_dir(void)
{
    return g_build_filename(g_get_user_config_dir(), "y720-control", NULL);
}

static gchar *config_path(void)
{
    gchar *dir = config_dir();
    gchar *path = g_build_filename(dir, "settings.ini", NULL);
    g_free(dir);
    return path;
}

gchar *y720_autostart_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "autostart", "org.y720.Control.desktop", NULL);
}

static gboolean ensure_dir(const gchar *path, GError **error)
{
    if (g_mkdir_with_parents(path, 0700) == 0)
        return TRUE;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                y720_tr("无法创建目录 %s：%s"), path, g_strerror(errno));
    return FALSE;
}

static void normalize_shifted_digit(gchar **accel)
{
    if (!accel || !*accel || !strstr(*accel, "<Shift>"))
        return;
    static const struct { const gchar *name; const gchar *digit; } map[] = {
        {"parenright", "0"}, {"exclam", "1"}, {"at", "2"},
        {"numbersign", "3"}, {"dollar", "4"}, {"percent", "5"},
        {"asciicircum", "6"}, {"ampersand", "7"}, {"asterisk", "8"},
        {"parenleft", "9"},
    };
    for (guint i = 0; i < G_N_ELEMENTS(map); i++) {
        if (g_str_has_suffix(*accel, map[i].name)) {
            gchar *base = g_strndup(*accel, strlen(*accel) - strlen(map[i].name));
            gchar *fixed = g_strconcat(base, map[i].digit, NULL);
            g_free(base);
            g_free(*accel);
            *accel = fixed;
            return;
        }
    }
}

void y720_settings_normalize_shortcuts(Y720Settings *settings)
{
    normalize_shifted_digit(&settings->profile_hotkey);
    normalize_shifted_digit(&settings->boost_hotkey);
}

Y720Settings *y720_settings_new(void)
{
    Y720Settings *settings = g_new0(Y720Settings, 1);
    settings->close_to_tray = TRUE;
    settings->global_hotkey = TRUE;
    settings->language = g_strdup("auto");
    settings->background_opacity = 0.45;
    settings->panel_opacity = 0.86;
    settings->window_opacity = 1.0;
    settings->accent = g_strdup("#c01c28");
    settings->profile_hotkey = g_strdup(Y720_DEFAULT_PROFILE_HOTKEY);
    settings->boost_hotkey = g_strdup(Y720_DEFAULT_BOOST_HOTKEY);
    const gint curve_defaults[6] = {1500, 1800, 2400, 3200, 4200, 5100};
    memcpy(settings->curve_points, curve_defaults, sizeof(curve_defaults));

    gchar *path = config_path();
    GKeyFile *keyfile = g_key_file_new();
    if (g_key_file_load_from_file(keyfile, path, G_KEY_FILE_NONE, NULL)) {
        settings->start_hidden = g_key_file_get_boolean(keyfile, "general", "start_hidden", NULL);
        settings->close_to_tray = g_key_file_get_boolean(keyfile, "general", "close_to_tray", NULL);
        settings->global_hotkey = g_key_file_get_boolean(keyfile, "general", "global_hotkey", NULL);
        settings->background_path = g_key_file_get_string(keyfile, "appearance", "background", NULL);
        settings->background_opacity = g_key_file_has_key(keyfile, "appearance", "background_opacity", NULL)
                                           ? g_key_file_get_double(keyfile, "appearance", "background_opacity", NULL)
                                           : 0.45;
        settings->panel_opacity = g_key_file_has_key(keyfile, "appearance", "panel_opacity", NULL)
                                      ? g_key_file_get_double(keyfile, "appearance", "panel_opacity", NULL)
                                      : 0.86;
        settings->window_opacity = g_key_file_has_key(keyfile, "appearance", "window_opacity", NULL)
                                       ? g_key_file_get_double(keyfile, "appearance", "window_opacity", NULL)
                                       : 1.0;
        settings->curve_enabled = g_key_file_get_boolean(keyfile, "fan", "curve_enabled", NULL);
        gsize curve_len = 0;
        gint *curve = g_key_file_get_integer_list(keyfile, "fan", "curve_points", &curve_len, NULL);
        if (curve && curve_len == 6) {
            for (guint i = 0; i < 6; i++)
                settings->curve_points[i] = CLAMP(curve[i], 1500, 5100);
        }
        g_free(curve);
        gchar *language = g_key_file_get_string(keyfile, "general", "language", NULL);
        if (language && *language) {
            g_free(settings->language);
            settings->language = language;
        } else {
            g_free(language);
        }
        gchar *boost_hotkey = g_key_file_get_string(keyfile, "shortcuts", "boost", NULL);
        if (boost_hotkey && *boost_hotkey) {
            g_free(settings->boost_hotkey);
            settings->boost_hotkey = boost_hotkey;
        } else {
            g_free(boost_hotkey);
        }
        gchar *hotkey = g_key_file_get_string(keyfile, "shortcuts", "profile", NULL);
        if (hotkey && *hotkey) {
            g_free(settings->profile_hotkey);
            settings->profile_hotkey = hotkey;
        } else {
            g_free(hotkey);
        }
        gchar *accent = g_key_file_get_string(keyfile, "appearance", "accent", NULL);
        if (accent && *accent) {
            g_free(settings->accent);
            settings->accent = accent;
        } else {
            g_free(accent);
        }
        settings->background_opacity = CLAMP(settings->background_opacity, 0.0, 1.0);
        settings->panel_opacity = CLAMP(settings->panel_opacity, 0.35, 1.0);
        settings->window_opacity = CLAMP(settings->window_opacity, 0.5, 1.0);

        y720_settings_normalize_shortcuts(settings);
        if (!valid_accel(settings->profile_hotkey)) {
            g_free(settings->profile_hotkey);
            settings->profile_hotkey = g_strdup(Y720_DEFAULT_PROFILE_HOTKEY);
        }
        if (!valid_accel(settings->boost_hotkey)) {
            g_free(settings->boost_hotkey);
            settings->boost_hotkey = g_strdup(Y720_DEFAULT_BOOST_HOTKEY);
        }
        gchar *effect = g_key_file_get_string(keyfile, "backlight", "effect", NULL);
        gsize count = 0;
        gint *colors = g_key_file_get_integer_list(keyfile, "backlight", "colors", &count, NULL);
        gsize bcount = 0;
        gint *brightness = g_key_file_get_integer_list(keyfile, "backlight", "brightness", &bcount, NULL);
        if (effect && colors && count == 4 && brightness && bcount == 4) {
            settings->has_profile = TRUE;
            settings->profile.effect = g_strdup(effect);
            memcpy(settings->profile.colors, colors, sizeof(settings->profile.colors));
            memcpy(settings->profile.brightness, brightness, sizeof(settings->profile.brightness));
        }
        g_free(effect);
        g_free(colors);
        g_free(brightness);
    }
    g_key_file_unref(keyfile);
    g_free(path);

    if (!settings->has_profile) {
        settings->has_profile = TRUE;
        settings->profile.effect = g_strdup("static");
        for (guint i = 0; i < 4; i++) {
            settings->profile.colors[i] = 0;
            settings->profile.brightness[i] = 2;
        }
    }
    return settings;
}

void y720_settings_free(Y720Settings *settings)
{
    if (!settings)
        return;
    y720_backlight_profile_clear(&settings->profile);
    g_free(settings->background_path);
    g_free(settings->accent);
    g_free(settings->profile_hotkey);
    g_free(settings->boost_hotkey);
    g_free(settings->language);
    g_free(settings);
}

void y720_settings_set_profile(Y720Settings *settings, const Y720BacklightProfile *profile)
{
    y720_backlight_profile_copy(&settings->profile, profile);
    settings->has_profile = TRUE;
}

void y720_settings_save(const Y720Settings *settings)
{
    GKeyFile *keyfile = g_key_file_new();
    g_key_file_set_boolean(keyfile, "general", "start_hidden", settings->start_hidden);
    g_key_file_set_boolean(keyfile, "general", "close_to_tray", settings->close_to_tray);
    g_key_file_set_boolean(keyfile, "general", "global_hotkey", settings->global_hotkey);
    g_key_file_set_string(keyfile, "general", "language",
                          settings->language ? settings->language : "auto");
    g_key_file_set_string(keyfile, "appearance", "background",
                          settings->background_path ? settings->background_path : "");
    g_key_file_set_double(keyfile, "appearance", "background_opacity", settings->background_opacity);
    g_key_file_set_double(keyfile, "appearance", "panel_opacity", settings->panel_opacity);
    g_key_file_set_double(keyfile, "appearance", "window_opacity", settings->window_opacity);
    g_key_file_set_string(keyfile, "appearance", "accent",
                          settings->accent ? settings->accent : "#c01c28");
    g_key_file_set_string(keyfile, "shortcuts", "profile",
                          settings->profile_hotkey ? settings->profile_hotkey : Y720_DEFAULT_PROFILE_HOTKEY);
    g_key_file_set_string(keyfile, "shortcuts", "boost",
                          settings->boost_hotkey ? settings->boost_hotkey : Y720_DEFAULT_BOOST_HOTKEY);
    g_key_file_set_boolean(keyfile, "fan", "curve_enabled", settings->curve_enabled);
    g_key_file_set_integer_list(keyfile, "fan", "curve_points", (gint *)settings->curve_points, 6);

    if (settings->has_profile) {
        g_key_file_set_string(keyfile, "backlight", "effect",
                              settings->profile.effect ? settings->profile.effect : "static");
        gint colors[4];
        gint brightness[4];
        memcpy(colors, settings->profile.colors, sizeof(colors));
        memcpy(brightness, settings->profile.brightness, sizeof(brightness));
        g_key_file_set_integer_list(keyfile, "backlight", "colors", colors, 4);
        g_key_file_set_integer_list(keyfile, "backlight", "brightness", brightness, 4);
    }

    gchar *data = g_key_file_to_data(keyfile, NULL, NULL);
    gchar *path = config_path();
    gchar *dir = config_dir();
    GError *error = NULL;
    if (ensure_dir(dir, &error)) {
        gchar *tmp = g_strconcat(path, ".tmp", NULL);
        if (g_file_set_contents(tmp, data, -1, NULL))
            rename(tmp, path);
        g_free(tmp);
    }
    if (error) {
        g_warning("settings save failed: %s", error->message);
        g_error_free(error);
    }
    g_free(dir);
    g_free(path);
    g_free(data);
    g_key_file_unref(keyfile);
}

static gchar *quoted_exec(const gchar *path)
{
    GString *out = g_string_new("\"");
    for (const gchar *p = path; *p; p++) {
        if (*p == '"' || *p == '\\')
            g_string_append_c(out, '\\');
        g_string_append_c(out, *p);
    }
    g_string_append_c(out, '"');
    return g_string_free(out, FALSE);
}

gboolean y720_autostart_enabled(void)
{
    gchar *path = y720_autostart_path();
    gchar *contents = NULL;
    gboolean enabled = g_file_get_contents(path, &contents, NULL, NULL) &&
                       strstr(contents, "X-Y720-Control-Autostart=true") != NULL;
    g_free(contents);
    g_free(path);
    return enabled;
}

gboolean y720_autostart_set(gboolean enabled, GError **error)
{
    gchar *path = y720_autostart_path();
    gboolean ok = TRUE;

    if (!enabled) {
        gchar *contents = NULL;
        if (!g_file_get_contents(path, &contents, NULL, NULL)) {
            ok = TRUE;
        } else if (!strstr(contents, "X-Y720-Control-Autostart=true")) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_EXIST,
                        y720_tr("自启动文件已存在且不是本程序创建的，未删除：%s"), path);
            ok = FALSE;
        } else if (unlink(path) != 0) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                        y720_tr("无法删除自启动文件：%s"), g_strerror(errno));
            ok = FALSE;
        }
        g_free(contents);
        g_free(path);
        return ok;
    }

    gchar *existing = NULL;
    if (g_file_get_contents(path, &existing, NULL, NULL)) {
        if (!strstr(existing, "X-Y720-Control-Autostart=true")) {
            g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_EXIST,
                        y720_tr("自启动文件已存在且不是本程序创建的，未覆盖：%s"), path);
            g_free(existing);
            g_free(path);
            return FALSE;
        }
        g_free(existing);
    }

    gchar *dir = g_path_get_dirname(path);
    if (!ensure_dir(dir, error)) {
        g_free(dir);
        g_free(path);
        return FALSE;
    }
    g_free(dir);

    gchar exe[4096];
    gssize length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (length <= 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, y720_tr("无法确定程序路径"));
        g_free(path);
        return FALSE;
    }
    exe[length] = '\0';
    gchar *quoted = quoted_exec(exe);

    gchar *contents = g_strdup_printf(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Y720 Control\n"
        "Comment=Start Y720 Control in the tray\n"
        "Exec=%s --hidden\n"
        "Icon=org.y720.Control\n"
        "Terminal=false\n"
        "X-GNOME-Autostart-enabled=true\n"
        "X-Y720-Control-Autostart=true\n",
        quoted);

    if (!g_file_set_contents(path, contents, -1, error))
        ok = FALSE;

    g_free(contents);
    g_free(quoted);
    g_free(path);
    return ok;
}
