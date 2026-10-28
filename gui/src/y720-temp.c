/* SPDX-License-Identifier: MIT */
#include "y720-temp.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

static gboolean read_millideg(const gchar *path, gint *value)
{
    FILE *file = fopen(path, "r");
    if (!file)
        return FALSE;
    long raw = 0;
    int ok = fscanf(file, "%ld", &raw);
    fclose(file);
    if (ok != 1 || raw <= 0 || raw > 200000)
        return FALSE;
    *value = (gint)(raw / 1000);
    return TRUE;
}

static gboolean known_sensor(const gchar *name)
{
    static const gchar *names[] = {
        "coretemp", "k10temp", "zenpower", "amdgpu", "nouveau", "nvidia",
        "cpu_thermal", "soc_thermal", "acpitz", NULL,
    };
    for (guint i = 0; names[i]; i++)
        if (g_strcmp0(name, names[i]) == 0)
            return TRUE;
    return FALSE;
}

gint y720_temp_read_max(void)
{
    gint best = -1;
    DIR *dir = opendir("/sys/class/hwmon");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (entry->d_name[0] == '.')
                continue;
            gchar *base = g_build_filename("/sys/class/hwmon", entry->d_name, NULL);
            gchar *name_path = g_build_filename(base, "name", NULL);
            gchar *name = NULL;
            if (g_file_get_contents(name_path, &name, NULL, NULL)) {
                g_strstrip(name);
                if (known_sensor(name)) {
                    for (gint i = 1; i <= 4; i++) {
                        gchar *p = g_strdup_printf("%s/temp%d_input", base, i);
                        gint value = 0;
                        if (read_millideg(p, &value) && value < 120 && value > best)
                            best = value;
                        g_free(p);
                    }
                }
                g_free(name);
            }
            g_free(name_path);
            g_free(base);
        }
        closedir(dir);
    }

    dir = opendir("/sys/class/thermal");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (!g_str_has_prefix(entry->d_name, "thermal_zone"))
                continue;
            gchar *base = g_build_filename("/sys/class/thermal", entry->d_name, NULL);
            gchar *type_path = g_build_filename(base, "type", NULL);
            gchar *type = NULL;
            if (g_file_get_contents(type_path, &type, NULL, NULL)) {
                g_strstrip(type);
                if (g_strcmp0(type, "x86_pkg_temp") == 0 ||
                    g_strcmp0(type, "acpitz") == 0) {
                    gchar *temp_path = g_build_filename(base, "temp", NULL);
                    gint value = 0;
                    if (read_millideg(temp_path, &value) && value < 120 && value > best)
                        best = value;
                    g_free(temp_path);
                }
                g_free(type);
            }
            g_free(type_path);
            g_free(base);
        }
        closedir(dir);
    }
    return best;
}
