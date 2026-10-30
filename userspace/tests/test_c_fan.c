/* SPDX-License-Identifier: MIT */
/* Compile-time syscall substitutions only. This executable never accesses hardware. */
#include "../common/cli.h"
#include <assert.h>
static int mock_read_text(const char *, char *, size_t);
static int mock_read_integer(const char *, long long *);
static int mock_stat(const char *, struct stat *);
static int mock_open(const char *, int, ...);
static ssize_t mock_write(int, const void *, size_t);
static int mock_close(int);
static DIR *mock_opendir(const char *);
static double mock_clock(void);
static int mock_sleep(double);
#define read_text mock_read_text
#define read_integer mock_read_integer
#define stat(path, info) mock_stat(path, info)
#define open mock_open
#define write mock_write
#define close mock_close
#define opendir mock_opendir
#define monotonic_seconds mock_clock
#define sleep_seconds mock_sleep
#define main fan_program_main
#include "../fan/y720_fan.c"
#undef main
#undef stat
#undef open
#undef write
#undef close
#undef opendir

static DIR *mock_opendir(const char *path)
{
    /* Never let optional telemetry tests depend on the build host's CPU. */
    if (!strcmp(path, "/sys/class/hwmon")) { errno = ENOENT; return NULL; }
    return opendir(path);
}

static int mode_value, target_value, pwm_value, writes, opens, failed_write, denied_open;
static int supersede, read_failure, status_reads, rpm_value, readonly, wrong_version;
static bool short_write, cancel_write;
static int cpu_mc, temp_error;
static int api_present, curve_owner, curve_fault, curve_replaced;
static int profile[6];
static const char *attribute;
static char written[8][32];
static double clock_value;

static int mock_read_text(const char *path, char *out, size_t capacity)
{
    if (strstr(path, "curve_backend")) { snprintf(out, capacity, "linux-thermal"); return 0; }
    if (strstr(path, "curve_profile")) {
        snprintf(out, capacity, "%d %d %d %d %d %d", profile[0], profile[1], profile[2], profile[3], profile[4],
                 curve_replaced && writes ? 5000 : profile[5]);
        return 0;
    }
    snprintf(out, capacity, "%s", strstr(path, "temperature_source") ? "x86_pkg_temp" : wrong_version ? "9.9" : CLI_VERSION);
    return 0;
}
static int mock_stat(const char *path, struct stat *info)
{
    (void)info;
    if (readonly || (strstr(path, "mockparent") && !(api_present && strstr(path, "control_api")))) {
        errno = ENOENT; return -1;
    }
    return 0;
}
static int mock_read_integer(const char *path, long long *value)
{
    if (strstr(path, "control_api")) *value = 2;
    else if (strstr(path, "curve_active")) *value = curve_owner;
    else if (strstr(path, "curve_error")) *value = curve_fault;
    else if (strstr(path, "curve_temperature")) *value = 55000;
    else if (strstr(path, "pwm1_enable")) {
        ++status_reads;
        if (status_reads == read_failure) { errno = EIO; return io_error("mock read"); }
        *value = mode_value;
    } else if (strstr(path, "fan1_target")) *value = supersede && status_reads >= 3 ? 2000 : target_value;
    else if (strstr(path, "pwm1")) *value = pwm_value;
    else if (strstr(path, "temp1_input")) {
        if (temp_error) { errno = EIO; return io_error("mock CPU unavailable"); }
        *value = cpu_mc;
    }
    else *value = rpm_value;
    return 0;
}
static int mock_open(const char *path, int flags, ...)
{
    (void)flags;
    ++opens;
    attribute = strrchr(path, '/') + 1;
    if (opens == denied_open) { errno = EACCES; return -1; }
    return 123;
}
static ssize_t mock_write(int fd, const void *payload, size_t length)
{
    assert(fd == 123 && writes < 8);
    strcpy(written[writes++], attribute);
    int value = atoi(payload);
    if (cancel_write) { cli_signal = SIGTERM; errno = EINTR; return -1; }
    if (writes == failed_write) { errno = ETIMEDOUT; return -1; }
    if (short_write) return 1;
    if (!strcmp(attribute, "curve_profile")) {
        assert(sscanf(payload, "%d %d %d %d %d %d", &profile[0], &profile[1], &profile[2], &profile[3], &profile[4], &profile[5]) == 6);
        curve_owner = 1;
        mode_value = 3;
    } else if (!strcmp(attribute, "pwm1_enable")) {
        mode_value = value;
        curve_owner = value == 3;
    }
    else {
        curve_owner = 0;
        if (mode_value == 3) mode_value = 1;
        target_value = !strcmp(attribute, "fan1_target") ? (value + 99) / 100 * 100 : 1500 + ((value * 36 + 254) / 255) * 100;
        pwm_value = !strcmp(attribute, "pwm1") ? value : (target_value - 1500) * 255 / 3600;
    }
    return (ssize_t)length;
}
static int mock_close(int fd) { assert(fd == 123); return 0; }
static double mock_clock(void) { return clock_value; }
static int mock_sleep(double duration) { clock_value += duration; return interrupted(); }
static void reset(int mode)
{
    cli_error = (struct cli_error){.completed = -1}; cli_signal = 0;
    mode_value = mode; target_value = rpm_value = 1500; pwm_value = 0;
    writes = opens = failed_write = denied_open = supersede = read_failure = status_reads = readonly = wrong_version = 0;
    short_write = cancel_write = false; clock_value = 0;
    cpu_mc = 45000; temp_error = 0;
    api_present = curve_owner = curve_fault = curve_replaced = 0;
    const int defaults[6] = {1500, 1800, 2400, 3200, 4200, 5100};
    memcpy(profile, defaults, sizeof(profile));
}
static int change(const struct request *request, const char *mode, double wait, bool *settled)
{
    struct fan_device device = { .path = "/mock", .parent = "/mockparent" };
    struct fan_state state;
    return fan_change(&device, request, mode, wait, 250, &state, settled);
}
int main(int argc, char **argv)
{
    if (argc > 1) {
        reset(1);
        readonly = !strcmp(argv[1], "readonly");
        if (!strcmp(argv[1], "thermal")) { api_present = curve_owner = 1; mode_value = 3; }
        struct fan_device device = {.path = "/mock", .parent = "/mockparent", .name = "hwmon7"};
        struct fan_state state = {0};
        setup("y720-fan", 0, NULL); cli_json = true;
        if (fan_status(&device, &state)) return report_error();
        envelope(stdout, true); print_status(&device, &state, true); fputs("}\n", stdout);
        assert(!writes && !opens);
        return flush_output(stdout);
    }
    /* Shared advisory lock safety on temporary regular files, never /run. */
    char directory[] = "/tmp/y720-c-lock-XXXXXX", path[PATH_MAX], link_path[PATH_MAX];
    assert(mkdtemp(directory));
    assert(!format_path(path, "%s/lock", directory));
    assert(!format_path(link_path, "%s/link", directory));
    int locked = writer_lock(path); assert(locked >= 0);
    assert(writer_lock(path) < 0 && cli_error.status == 5);
    assert(!close(locked));
    assert(!symlink(path, link_path)); assert(writer_lock(link_path) < 0);
    assert(!unlink(link_path));
    assert(!chmod(path, 0644)); assert(writer_lock(path) < 0 && cli_error.status == 5);
    assert(!chmod(path, 0600)); assert(!link(path, link_path));
    assert(writer_lock(path) < 0 && cli_error.status == 5);
    assert(!unlink(link_path)); assert(!unlink(path)); assert(!rmdir(directory));
    bool settled;
    struct fan_device temps_device = {.path="/mock", .parent="/mockparent"};
    struct fan_state temps;
    const int points[6] = {1500, 1800, 2400, 3200, 4200, 5100};
    reset(2);
    assert(curve_change(&temps_device, true, points, &temps) < 0 && cli_error.status == 3 && !writes);
    reset(2); api_present = 1;
    assert(!curve_change(&temps_device, true, points, &temps) && writes == 1 && temps.curve_active && mode_value == 3);
    assert(!strcmp(written[0], "curve_profile"));
    assert(!curve_change(&temps_device, false, points, &temps) && writes == 2 && !temps.curve_active && mode_value == 1);
    assert(!curve_change(&temps_device, false, points, &temps) && writes == 2); /* Stop is idempotent. */
    reset(2); api_present = 1; failed_write = 1;
    assert(curve_change(&temps_device, true, points, &temps) < 0 && cli_error.status == 7 && mode_value == 2 && writes == 1);
    reset(2); api_present = 1; curve_replaced = 1;
    assert(curve_change(&temps_device, true, points, &temps) < 0 && cli_error.status == 5 && writes == 1);
    reset(3); api_present = curve_owner = 1; curve_fault = -61;
    assert(!fan_status(&temps_device, &temps) && temps.curve_active && temps.curve_error == -61 && !writes);
    reset(1);
    assert(!fan_status(&temps_device, &temps) && temps.temp_valid);
    assert(temps.temp_mc == 45000 && !strcmp(temps.temp_source, "x86_pkg_temp"));
    temp_error = 1;
    assert(!fan_status(&temps_device, &temps) && !temps.temp_valid);
    assert(!cli_error.status);
    struct request request = {.present = true, .by_rpm = true, .value = 4001, .target = 4100, .pwm = 184};
    reset(2);
    assert(change(&request, NULL, 0, &settled) == 0 && !settled);
    assert(writes == 2 && !strcmp(written[0], "fan1_target") && !strcmp(written[1], "pwm1_enable"));
    reset(1); assert(!change(&request, NULL, 0, &settled) && writes == 1);
    reset(3); api_present = curve_owner = 1;
    assert(!change(&request, NULL, 0, &settled) && writes == 1 && mode_value == 1 && !curve_owner);
    for (int pwm = 0; pwm <= 255; ++pwm) {
        reset(1);
        request = (struct request){.present = true, .value = pwm, .pwm = pwm, .target = 1500 + ((pwm * 36 + 254) / 255) * 100};
        assert(!change(&request, NULL, 0, &settled) && writes == 1);
    }
    request = (struct request){.present = true, .by_rpm = true, .value = 1500, .target = 1500, .pwm = 0};
    reset(1); assert(!change(&request, NULL, 2, &settled) && settled && clock_value == 1);
    reset(1); rpm_value = 0; assert(change(&request, NULL, 1, &settled) == -1 && cli_error.status == 8 && writes == 1 && mode_value == 1);
    reset(1); supersede = 1; assert(change(&request, NULL, 2, &settled) == -1 && cli_error.status == 5 && writes == 1);
    for (int position = 1; position <= 2; ++position) {
        reset(2); failed_write = position;
        assert(change(&request, NULL, 0, &settled) == -1 && cli_error.status == 7 && cli_error.uncertain && writes == position);
    }
    reset(2); denied_open = 1; assert(change(&request, NULL, 0, &settled) == -1 && cli_error.status == 4 && writes == 0);
    reset(2); denied_open = 2; assert(change(&request, NULL, 0, &settled) == -1 && cli_error.status == 7 && writes == 1);
    reset(1); short_write = true; assert(change(&request, NULL, 0, &settled) == -1 && cli_error.status == 7 && writes == 1);
    reset(1); cancel_write = true; assert(change(&request, NULL, 0, &settled) == -1 && cli_error.status == 143 && writes == 1);
    for (int position = 2; position <= 3; ++position) {
        reset(1); read_failure = position;
        assert(change(&request, NULL, 2, &settled) == -1 && cli_error.status == 7 && writes == 1);
    }
    struct request no_target = {0};
    reset(1); assert(!change(&no_target, "auto", 0, &settled) && mode_value == 2 && writes == 1);
    reset(1); assert(!change(&no_target, "boost", 0, &settled) && mode_value == 0 && writes == 1);
    reset(1); readonly = 1; assert(change(&request, NULL, 0, &settled) == -1 && cli_error.status == 3 && writes == 0);
    reset(1); wrong_version = 1; assert(change(&request, NULL, 0, &settled) == -1 && cli_error.status == 3 && writes == 0);
    puts("PASS C fan: 256 PWM requests, mode/order/readback/wait/supersede/permission/short-write/cancel failures; no implicit fallback");
    return 0;
}
