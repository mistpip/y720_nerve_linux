/* SPDX-License-Identifier: MIT */
/* Read-only compatibility with modules predating Y720 temperature attributes.
 * Match the sensor identity, never a reusable hwmon number or a PCH sensor.
 */
static int package_temperature(const char *root, long long *value)
{
    DIR *directory = opendir(root);
    if (!directory) return -1;
    int result = -1;
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        char path[PATH_MAX], name[96], label[96];
        long long sample;
        if (!numbered_name(entry->d_name, "hwmon")) continue;
        if (format_path(path, "%s/%s/name", root, entry->d_name) ||
            read_text(path, name, sizeof(name)) || strcmp(name, "coretemp")) continue;
        if (format_path(path, "%s/%s/temp1_label", root, entry->d_name) ||
            read_text(path, label, sizeof(label)) || strcmp(label, "Package id 0")) continue;
        if (format_path(path, "%s/%s/temp1_input", root, entry->d_name) ||
            read_integer(path, &sample) || sample < 1000 || sample > 120000) continue;
        *value = sample;
        result = 0;
        break;
    }
    closedir(directory);
    return result;
}
