/* SPDX-License-Identifier: MIT */
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include "y720-settings.h"
#include "y720-i18n.h"

int main(void)
{
    g_setenv("LANGUAGE", "zh_CN", TRUE);
    y720_i18n_prepare_environment("en");
    g_assert_cmpstr(g_getenv("LANGUAGE"), ==, "en");
    gchar *dir = g_dir_make_tmp("y720-settings-test-XXXXXX", NULL);
    g_assert_nonnull(dir);
    g_setenv("XDG_CONFIG_HOME", dir, TRUE);
    Y720Settings *settings = y720_settings_new();
    g_assert_cmpstr(settings->profile_hotkey, ==, "evdev:228");
    g_assert_cmpstr(settings->boost_hotkey, ==, "<Control><Shift>0");
    y720_settings_save(settings);
    y720_settings_free(settings);
    settings = y720_settings_new();
    g_assert_cmpstr(settings->profile_hotkey, ==, "evdev:228");
    g_assert_cmpstr(settings->boost_hotkey, ==, "<Control><Shift>0");
    g_free(settings->profile_hotkey);
    settings->profile_hotkey = g_strdup("<Control><Alt>p");
    g_free(settings->boost_hotkey);
    settings->boost_hotkey = g_strdup("<Shift><Control>parenright");
    y720_settings_save(settings);
    y720_settings_free(settings);
    settings = y720_settings_new();
    /* Preserve explicit user choices and normalize the shifted top-row zero. */
    g_assert_cmpstr(settings->profile_hotkey, ==, "<Control><Alt>p");
    g_assert_cmpstr(settings->boost_hotkey, ==, "<Shift><Control>0");
    g_free(settings->profile_hotkey);
    settings->profile_hotkey = g_strdup("evdev:228junk");
    g_free(settings->boost_hotkey);
    settings->boost_hotkey = g_strdup("keycode:4294967297");
    y720_settings_save(settings);
    y720_settings_free(settings);
    settings = y720_settings_new();
    g_assert_cmpstr(settings->profile_hotkey, ==, Y720_DEFAULT_PROFILE_HOTKEY);
    g_assert_cmpstr(settings->boost_hotkey, ==, Y720_DEFAULT_BOOST_HOTKEY);
    y720_settings_free(settings);
    gchar *config = g_build_filename(dir, "y720-control", NULL);
    gchar *file = g_build_filename(config, "settings.ini", NULL);
    g_unlink(file);
    g_rmdir(config);
    g_rmdir(dir);
    g_free(file); g_free(config); g_free(dir);
    return 0;
}
