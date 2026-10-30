/* SPDX-License-Identifier: MIT */
#define main fan_program_main
#include "../fan/y720_fan.c"
#undef main
#include <assert.h>

static void put(const char *directory, const char *name, const char *text)
{
    char path[PATH_MAX];
    assert(!format_path(path, "%s/%s", directory, name));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    assert(fd >= 0);
    assert(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    assert(!close(fd));
}

int main(void)
{
    char root[] = "/tmp/y720-temperature-XXXXXX", sensor[PATH_MAX], path[PATH_MAX];
    assert(mkdtemp(root));
    assert(!format_path(sensor, "%s/hwmon99", root));
    assert(!mkdir(sensor, 0700));
    long long value = 0;
    put(sensor, "name", "pch_skylake\n");
    put(sensor, "temp1_label", "Package id 0\n");
    put(sensor, "temp1_input", "45000\n");
    assert(package_temperature(root, &value) < 0);
    put(sensor, "name", "coretemp\n");
    assert(!package_temperature(root, &value) && value == 45000);
    put(sensor, "temp1_label", "Core 0\n");
    assert(package_temperature(root, &value) < 0);
    put(sensor, "temp1_label", "Package id 0\n");
    const char *bad[] = {"0", "-1000", "120001", "nan", "45000extra"};
    for (size_t i = 0; i < ARRAY_SIZE(bad); ++i) {
        put(sensor, "temp1_input", bad[i]);
        assert(package_temperature(root, &value) < 0);
    }
    assert(!format_path(path, "%s/temp1_input", sensor));
    int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    assert(fd >= 0 && write(fd, "45000\0extra", 11) == 11 && !close(fd));
    assert(package_temperature(root, &value) < 0);
    put(sensor, "temp1_input", "61000\n");
    assert(!package_temperature(root, &value) && value == 61000);
    const char *files[] = {"name", "temp1_label", "temp1_input"};
    for (size_t i = 0; i < ARRAY_SIZE(files); ++i) {
        assert(!format_path(path, "%s/%s", sensor, files[i]));
        assert(!unlink(path));
    }
    assert(!rmdir(sensor));
    assert(package_temperature(root, &value) < 0);
    assert(!rmdir(root));
    puts("PASS CPU telemetry: sensor identity, renumbering, live samples, invalid/missing readings");
    return 0;
}
