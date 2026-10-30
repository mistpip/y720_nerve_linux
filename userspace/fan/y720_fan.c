/* SPDX-License-Identifier: MIT */
/* Linked hwmon client only. Never accesses the EC or implements a curve daemon. */
#include "../common/cli.h"
#include "temperature.h"

struct fan_device { char path[PATH_MAX], parent[PATH_MAX], name[64]; bool attempted; };
struct fan_state {
    bool controls, diagnostic_present[6];
    char version[96];
    long long mode, target, pwm, rpm[2], diagnostic[6];
    bool temp_valid;
    long long temp_mc;
    const char *temp_source;
    long long control_api, curve_active, curve_error, curve_temp;
    bool curve_supported, curve_temp_valid;
    int curve_points[6];
};
struct request { bool present, by_rpm; int value, target, pwm; };
static const char *const diagnostic_names[] = {
    "requested_mode", "safety_fault", "last_error", "target_writes", "mode_writes", "ramp_low_samples"
};

static int fan_discover(struct fan_device *fan, const char *selected)
{
    if (verify_dmi()) return -1;
    DIR *directory = opendir("/sys/class/hwmon");
    if (!directory) return errno == ENOENT ? fail("device_not_found", 3, "No hwmon directory") : io_error("list hwmon");
    int matches = 0, result = -1;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(directory);
        if (!entry) { if (errno) { io_error("enumerate hwmon"); goto done; } break; }
        if (!numbered_name(entry->d_name, "hwmon") || (selected && strcmp(selected, entry->d_name))) continue;
        char path[PATH_MAX], text[96], resolved[PATH_MAX], parent[PATH_MAX], hwmon_path[PATH_MAX];
        if (format_path(path, "/sys/class/hwmon/%s/name", entry->d_name)) goto done;
        if (read_text(path, text, sizeof(text))) {
            if (cli_error.errnum == ENOENT) { cli_error = (struct cli_error){.completed = -1}; continue; }
            goto done;
        }
        if (strcmp(text, "y720_cooling")) continue;
        if (format_path(path, "/sys/class/hwmon/%s", entry->d_name)) goto done;
        if (!realpath(path, hwmon_path)) { io_error("resolve hwmon"); goto done; }
        strcpy(parent, hwmon_path);
        for (int i = 0; i < 2; ++i) {
            char *slash = strrchr(parent, '/');
            if (!slash || slash == parent) { fail("unsupported_device", 3, "Unexpected hwmon parent"); goto done; }
            *slash = 0;
        }
        if (format_path(path, "%s/driver", parent)) goto done;
        if (!realpath(path, resolved)) {
            if (errno == ENOENT) continue;
            io_error("resolve driver"); goto done;
        }
        if (strcmp(strrchr(resolved, '/') + 1, "y720_cooling")) continue;
        ++matches;
        strcpy(fan->name, entry->d_name);
        strcpy(fan->parent, parent);
        /* Pin the verified sysfs path rather than a reusable class number. */
        strcpy(fan->path, hwmon_path);
    }
    result = matches == 1 ? 0 : fail("device_not_found", 3, "Expected one verified Y720 hwmon, found %d", matches);
done:
    closedir(directory);
    return result;
}

static int fan_integer(const char *directory, const char *name, long long *value)
{
    char path[PATH_MAX];
    if (format_path(path, "%s/%s", directory, name)) return -1;
    return read_integer(path, value);
}

static int exists_at(const char *directory, const char *name, bool *exists)
{
    char path[PATH_MAX];
    struct stat info;
    if (format_path(path, "%s/%s", directory, name)) return -1;
    if (!stat(path, &info)) { *exists = true; return 0; }
    if (errno == ENOENT) { *exists = false; return 0; }
    return io_error("stat sysfs attribute");
}

static int parse_curve(char *text, int points[6])
{
    char *cursor = text;
    for (int i = 0; i < 6; ++i) {
        while (isspace((unsigned char)*cursor)) cursor++;
        char *end;
        errno = 0;
        long value = strtol(cursor, &end, 10);
        if (end == cursor || errno || value < 1500 || value > 5100 || value % 100 ||
            (*end && !isspace((unsigned char)*end)))
            return fail("usage", 2, "Expected six RPM values, 1500..5100 in steps of 100");
        points[i] = (int)value;
        if (i && points[i] < points[i - 1])
            return fail("usage", 2, "Curve RPM must not decrease as temperature rises");
        cursor = end;
    }
    while (isspace((unsigned char)*cursor)) cursor++;
    return *cursor ? fail("usage", 2, "Unexpected data after six curve points") : 0;
}

static int fan_status(const struct fan_device *fan, struct fan_state *state)
{
    memset(state, 0, sizeof(*state));
    state->controls = true;
    const char *const controls[] = {"pwm1_enable", "pwm1", "fan1_target"};
    for (size_t i = 0; i < ARRAY_SIZE(controls); ++i) {
        bool exists = false;
        if (exists_at(fan->path, controls[i], &exists)) return -1;
        state->controls &= exists;
    }
    if (read_text("/sys/module/y720_cooling/version", state->version, sizeof(state->version))) return -1;
    if (state->controls) {
        if (fan_integer(fan->path, "pwm1_enable", &state->mode) ||
            fan_integer(fan->path, "fan1_target", &state->target) ||
            fan_integer(fan->path, "pwm1", &state->pwm)) return -1;
        if (state->mode < 0 || state->mode > 3) return fail("invalid_readback", 6, "Unknown hardware mode");
    }
    if (fan_integer(fan->path, "fan1_input", &state->rpm[0]) ||
        fan_integer(fan->path, "fan2_input", &state->rpm[1])) return -1;
    for (size_t i = 0; i < ARRAY_SIZE(diagnostic_names); ++i) {
        if (exists_at(fan->parent, diagnostic_names[i], &state->diagnostic_present[i])) return -1;
        if (state->diagnostic_present[i] && fan_integer(fan->parent, diagnostic_names[i], &state->diagnostic[i])) return -1;
    }
    /* Optional telemetry failures do not suppress otherwise valid tach data. */
    struct cli_error saved_error = cli_error;
    char path[PATH_MAX], source[48];
    bool kernel_sensor = !format_path(path, "%s/temperature_source", fan->parent) &&
        !read_text(path, source, sizeof(source)) && !strcmp(source, "x86_pkg_temp");
    cli_error = saved_error;
    if (!fan_integer(fan->path, "temp1_input", &state->temp_mc) &&
        state->temp_mc >= 1000 && state->temp_mc <= 120000) {
        state->temp_valid = true;
        state->temp_source = kernel_sensor ? "x86_pkg_temp" : "coretemp";
    }
    cli_error = saved_error;
    if (!state->temp_valid && !package_temperature("/sys/class/hwmon", &state->temp_mc)) {
        state->temp_valid = true;
        state->temp_source = "coretemp";
    }
    cli_error = saved_error;
    bool api_exists = false;
    if (exists_at(fan->parent, "control_api", &api_exists)) return -1;
    if (api_exists && fan_integer(fan->parent, "control_api", &state->control_api)) return -1;
    state->curve_supported = state->controls && state->control_api == 2 && kernel_sensor;
    if (state->curve_supported) {
        char profile[128], backend[32];
        if (format_path(path, "%s/curve_backend", fan->parent) || read_text(path, backend, sizeof(backend))) return -1;
        if (strcmp(backend, "linux-thermal"))
            return fail("unsupported_version", 3, "Unknown kernel curve backend");
        if (fan_integer(fan->parent, "curve_active", &state->curve_active) ||
            fan_integer(fan->parent, "curve_error", &state->curve_error) ||
            format_path(path, "%s/curve_profile", fan->parent) || read_text(path, profile, sizeof(profile)) ||
            parse_curve(profile, state->curve_points)) return -1;
        if (state->curve_active < 0 || state->curve_active > 1)
            return fail("invalid_readback", 6, "Unknown curve owner");
        if (!fan_integer(fan->parent, "curve_temperature", &state->curve_temp))
            state->curve_temp_valid = state->curve_temp >= 1000 && state->curve_temp <= 120000;
        cli_error = saved_error;
    }
    return interrupted();
}

static const char *mode_name(long long mode)
{
    return mode == 0 ? "boost" : mode == 1 || mode == 3 ? "curve" : "auto";
}

static void print_status(const struct fan_device *fan, const struct fan_state *state, bool object)
{
    const char *separator = object ? "," : "\n";
    if (object) fputc('{', stdout);
    fputs(object ? "\"device\":" : "device=", stdout); quote(stdout, fan->name);
    fputs(separator, stdout); fputs(object ? "\"driver_version\":" : "driver_version=", stdout); quote(stdout, state->version);
    fprintf(stdout, "%s%s%s", separator, object ? "\"controls_available\":" : "controls_available=", state->controls ? "true" : "false");
    fputs(separator, stdout); fputs(object ? "\"mode\":" : "mode=", stdout);
    if (state->controls) quote(stdout, mode_name(state->mode)); else fputs("null", stdout);
    const char *const names[] = {"mode_raw", "target_rpm", "pwm"};
    long long values[] = {state->mode, state->target, state->pwm};
    for (size_t i = 0; i < ARRAY_SIZE(names); ++i) {
        fputs(separator, stdout);
        if (object) { quote(stdout, names[i]); fputc(':', stdout); } else fprintf(stdout, "%s=", names[i]);
        if (state->controls) fprintf(stdout, "%lld", values[i]); else fputs("null", stdout);
    }
    fprintf(stdout, "%s%s[%lld,%lld]%s%s{", separator, object ? "\"fan_rpm\":" : "fan_rpm=",
            state->rpm[0], state->rpm[1], separator, object ? "\"diagnostics\":" : "diagnostics=");
    for (size_t i = 0; i < ARRAY_SIZE(diagnostic_names); ++i) {
        if (i) fputc(',', stdout);
        quote(stdout, diagnostic_names[i]); fputc(':', stdout);
        if (state->diagnostic_present[i]) fprintf(stdout, "%lld", state->diagnostic[i]); else fputs("null", stdout);
    }
    fputc('}', stdout);
    fprintf(stdout, "%s%s{\"millidegrees\":", separator,
            object ? "\"temperature\":" : "temperature=");
    if (state->temp_valid) fprintf(stdout, "%lld", state->temp_mc); else fputs("null", stdout);
    fputs(",\"source\":", stdout);
    if (state->temp_source) quote(stdout, state->temp_source); else fputs("null", stdout);
    fputc('}', stdout);
    fprintf(stdout, "%s%s{\"supported\":%s,\"active\":%s,\"engine\":%s,\"error\":%lld,\"points\":[",
            separator, object ? "\"kernel_curve\":" : "kernel_curve=",
            state->curve_supported ? "true" : "false", state->curve_active ? "true" : "false",
            state->curve_supported ? "\"linux-thermal\"" : "null", state->curve_error);
    for (int i = 0; i < 6; ++i) fprintf(stdout, "%s%d", i ? "," : "", state->curve_points[i]);
    fputs("],\"temperature_millidegrees\":", stdout);
    if (state->curve_temp_valid) fprintf(stdout, "%lld", state->curve_temp); else fputs("null", stdout);
    fputc('}', stdout);
    if (object) fputc('}', stdout); else fputc('\n', stdout);
}

static int curve_change(struct fan_device *fan, bool apply, const int points[6], struct fan_state *state)
{
    if (fan_status(fan, state)) return -1;
    if (!state->curve_supported || strcmp(state->version, CLI_VERSION))
        return fail("unsupported_version", 3, "Kernel curve requires matching driver with control_api=2");
    if (!apply && !state->curve_active) return 0;
    char path[PATH_MAX], payload[128];
    if (format_path(path, "%s/%s", apply ? fan->parent : fan->path, apply ? "curve_profile" : "pwm1_enable")) return -1;
    int length = apply ? snprintf(payload, sizeof(payload), "%d %d %d %d %d %d\n",
        points[0], points[1], points[2], points[3], points[4], points[5]) : snprintf(payload, sizeof(payload), "1\n");
    int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return io_error("open kernel curve control");
    if (interrupted()) { close(fd); return -1; }
    fan->attempted = true;
    ssize_t count = write(fd, payload, (size_t)length);
    int saved = errno, closed = close(fd);
    if (count != length || closed) {
        errno = count < 0 ? saved : EIO;
        io_error("commit kernel curve");
        cli_error.uncertain = true;
        return fail("write_uncertain", 7, "Kernel curve submission failed; read status, no automatic fallback");
    }
    if (fan_status(fan, state)) return -1;
    if ((bool)state->curve_active != apply || (apply && memcmp(points, state->curve_points, sizeof(state->curve_points))))
        return fail("superseded", 5, "Kernel curve configuration changed by another controller");
    return interrupted();
}

static void print_request(const struct request *desired, const char *mode)
{
    if (desired->present)
        fprintf(stdout, "{\"target_rpm\":%d,\"pwm\":%d,\"kind\":\"%s\",\"value\":%d}",
                desired->target, desired->pwm, desired->by_rpm ? "rpm" : "pwm", desired->value);
    else { fputs("{\"mode\":", stdout); quote(stdout, mode); fputc('}', stdout); }
}

static int fan_write(struct fan_device *fan, const char *name, int value)
{
    char path[PATH_MAX], payload[32];
    if (format_path(path, "%s/%s", fan->path, name) || interrupted()) return -1;
    int size = snprintf(payload, sizeof(payload), "%d\n", value);
    int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return io_error("open sysfs for write");
    if (interrupted()) { close(fd); return -1; }
    fan->attempted = true;
    ssize_t count = write(fd, payload, (size_t)size);
    int saved = errno;
    int closed = close(fd);
    if (count != size) { errno = count < 0 ? saved : EIO; return io_error("write sysfs"); }
    if (closed) return io_error("close sysfs");
    return interrupted();
}

static int write_uncertain(void)
{
    cli_error.uncertain = true;
    if (cli_signal) return interrupted();
    char message[sizeof(cli_error.message)];
    strcpy(message, cli_error.message);
    return fail("write_uncertain", 7, "%s", message);
}

static int fan_change(struct fan_device *fan, const struct request *desired, const char *mode,
                      double wait, int tolerance, struct fan_state *current, bool *settled)
{
    if (!desired->present && (!mode || (strcmp(mode, "auto") && strcmp(mode, "boost"))))
        return fail("usage", 2, "An explicit target or factory mode is required");
    if (fan_status(fan, current)) return -1;
    if (!current->controls) return fail("read_only", 3, "Driver is read-only; explicitly reload with allow_control=1");
    if (strcmp(current->version, CLI_VERSION)) return fail("unsupported_version", 3, "Writes require driver " CLI_VERSION);
    int expected = desired->present ? 1 : !strcmp(mode, "auto") ? 2 : 0;
    fan->attempted = false;
    if (desired->present) {
        cli_error.phase = "target_write";
        if (fan_write(fan, desired->by_rpm ? "fan1_target" : "pwm1", desired->value)) goto failed;
        if (current->mode != 1 && current->mode != 3) {
            cli_error.phase = "enable_curve";
            if (fan_write(fan, "pwm1_enable", 1)) goto failed;
        }
    } else {
        cli_error.phase = "mode_write";
        if (fan_write(fan, "pwm1_enable", expected)) goto failed;
    }
    cli_error.phase = "readback";
    if (fan_status(fan, current)) goto failed;
    if (current->mode != expected || (desired->present &&
        (current->target != desired->target || current->pwm != desired->pwm))) {
        fail("readback_mismatch", 7, "Request readback differs; possible competing controller"); goto failed;
    }
    *settled = false;
    if (desired->present && wait > 0) {
        double now = monotonic_seconds();
        if (now < 0) { io_error("read clock"); goto failed; }
        double deadline = now + wait;
        unsigned consecutive = 0;
        cli_error.phase = "wait_readback";
        for (;;) {
            if (fan_status(fan, current)) goto failed;
            if (current->mode != 1 || current->target != desired->target) {
                cli_error.uncertain = true;
                return fail("superseded", 5, "Mode/target changed by another controller");
            }
            /* Avoid signed-overflow from a malformed readback. */
            bool close_enough = true;
            for (size_t i = 0; i < 2; ++i)
                close_enough &= current->rpm[i] >= desired->target - tolerance && current->rpm[i] <= desired->target + tolerance;
            consecutive = close_enough ? consecutive + 1 : 0;
            if (consecutive >= 2) { *settled = true; break; }
            now = monotonic_seconds();
            if (now < 0) { io_error("read clock"); goto failed; }
            if (now >= deadline) return fail("timeout", 8, "RPM did not settle; latest request remains, no rollback");
            if (sleep_seconds(fmin(1.0, deadline - now))) goto failed;
        }
    }
    cli_error.phase = NULL;
    return 0;
failed:
    return fan->attempted ? write_uncertain() : -1;
}

static void help(void)
{
    puts("y720-fan " CLI_VERSION " — standard hwmon client\n"
         "Usage: y720-fan [--json] [--dry-run] [--device hwmonN] COMMAND\n"
         "  status\n  mode auto|boost\n"
         "  curve apply -  (stdin: six RPM values at 40/50/60/70/80/90 C)\n  curve stop\n"
         "  set (--rpm 1500..5100 | --pwm 0..255) [--wait SECONDS] [--tolerance RPM]\n"
         "Wait: finite 1..120 seconds; tolerance: 0..500, default 250.\n"
         "RPM rounds upward to 100. PWM is normalized, not electrical duty cycle.\n"
         "Writes are silent unless --json or --dry-run; no automatic mode fallback.");
}

static int run(int argc, char **argv)
{
    struct global_options options = {0};
    struct request desired = {0};
    double wait = 0;
    int tolerance = 250;
    bool tolerance_set = false, wait_set = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { help(); return flush_output(stdout); }
        if (!strcmp(argv[i], "--version")) { puts("y720-fan " CLI_VERSION); return flush_output(stdout); }
    }
    if (globals(argc, argv, &options)) return -1;
    if (options.device && !numbered_name(options.device, "hwmon")) return fail("usage", 2, "--device must be a hwmonN name");
    int i = options.command_index;
    if (i >= argc) return fail("usage", 2, "A command is required; see --help");
    const char *command = argv[i++], *mode = NULL;
    bool curve_command = !strcmp(command, "curve"), curve_apply = false;
    int points[6] = {0};
    if (curve_command) {
        if (i >= argc) return fail("usage", 2, "curve requires apply - or stop");
        if (!strcmp(argv[i], "apply") && i + 2 == argc && !strcmp(argv[i + 1], "-")) {
            char input[128]; size_t used = 0;
            for (;;) {
                if (interrupted()) return -1;
                ssize_t got = read(STDIN_FILENO, input + used, sizeof(input) - 1 - used);
                if (got < 0) { if (errno == EINTR) continue; return io_error("read curve stdin"); }
                if (!got) break;
                used += (size_t)got;
                if (used == sizeof(input) - 1) return fail("usage", 2, "Curve input too large");
            }
            if (memchr(input, 0, used)) return fail("usage", 2, "NUL in curve input");
            input[used] = 0;
            if (parse_curve(input, points)) return -1;
            curve_apply = true;
        } else if (strcmp(argv[i], "stop") || i + 1 != argc) return fail("usage", 2, "curve requires apply - or stop");
        i = argc;
    } else if (!strcmp(command, "mode")) {
        if (i >= argc || (strcmp(argv[i], "auto") && strcmp(argv[i], "boost")))
            return fail("usage", 2, "mode requires auto or boost");
        mode = argv[i++];
    } else if (!strcmp(command, "set")) {
        for (; i < argc; ++i) {
            char option[32];
            const char *equal = strchr(argv[i], '=');
            size_t length = equal ? (size_t)(equal - argv[i]) : strlen(argv[i]);
            if (length >= sizeof(option)) return fail("usage", 2, "Unknown set option");
            memcpy(option, argv[i], length); option[length] = 0;
            if (strcmp(option, "--rpm") && strcmp(option, "--pwm") && strcmp(option, "--wait") && strcmp(option, "--tolerance"))
                return fail("usage", 2, "Unknown set option");
            const char *value = equal ? equal + 1 : ++i < argc ? argv[i] : NULL;
            if (!value) return fail("usage", 2, "Option requires a value");
            if (!strcmp(option, "--wait")) {
                char *end;
                errno = 0; wait = strtod(value, &end);
                while (isspace((unsigned char)*end)) ++end;
                if (end == value || *end || errno == ERANGE || !isfinite(wait) || wait < 1 || wait > 120)
                    return fail("usage", 2, "--wait must be finite and 1..120 seconds");
                if (strpbrk(value, "xXpP")) return fail("usage", 2, "--wait requires a decimal number");
                wait_set = true;
            } else {
                long long number;
                bool rpm = !strcmp(option, "--rpm"), pwm = !strcmp(option, "--pwm");
                if (parse_integer(value, rpm ? 1500 : 0, rpm ? 5100 : pwm ? 255 : 500, &number)) return fail("usage", 2, "Integer option is out of range");
                if (rpm || pwm) {
                    if (desired.present) return fail("usage", 2, "Specify exactly one target");
                    desired = (struct request){.present = true, .by_rpm = rpm, .value = (int)number};
                    desired.target = rpm ? ((desired.value + 99) / 100) * 100 : 1500 + ((desired.value * 36 + 254) / 255) * 100;
                    desired.pwm = pwm ? desired.value : (desired.target - 1500) * 255 / 3600;
                } else { tolerance = (int)number; tolerance_set = true; }
            }
        }
        if (!desired.present || (tolerance_set && !wait_set)) return fail("usage", 2, "Target required; --tolerance requires --wait");
    } else if (strcmp(command, "status")) return fail("usage", 2, "Unknown command");
    if (i != argc || (options.dry_run && !strcmp(command, "status"))) return fail("usage", 2, "Unexpected arguments or read-only dry-run");
    if (interrupted()) return -1;
    struct fan_device fan = {0};
    struct fan_state state = {0};
    bool settled = false;
    if (!options.dry_run) {
        if (fan_discover(&fan, options.device)) return -1;
        if (!strcmp(command, "status")) { if (fan_status(&fan, &state)) return -1; }
        else {
            int lock = writer_lock("/run/y720-fan.lock");
            if (lock < 0) return -1;
            int result = curve_command ? curve_change(&fan, curve_apply, points, &state) :
                fan_change(&fan, &desired, mode, wait, tolerance, &state, &settled);
            close(lock);
            if (result) return -1;
        }
    }
    if (interrupted()) return -1;
    if (!cli_json && !options.dry_run && strcmp(command, "status")) return 0;
    if (cli_json) envelope(stdout, true);
    if (!strcmp(command, "status")) print_status(&fan, &state, cli_json);
    else {
        fputs(cli_json ? "{\"operation\":" : "operation=", stdout); quote(stdout, command);
        fputs(cli_json ? ",\"request\":" : "\nrequest=", stdout);
        if (curve_command) {
            fprintf(stdout, "{\"active\":%s", curve_apply ? "true" : "false");
            if (curve_apply) {
                fputs(",\"points\":[", stdout);
                for (int p = 0; p < 6; p++) fprintf(stdout, "%s%d", p ? "," : "", points[p]);
                fputc(']', stdout);
            }
            fputc('}', stdout);
        } else print_request(&desired, mode);
        if (options.dry_run) fputs(cli_json ? ",\"device_verified\":false,\"hardware_written\":false}" : "\ndevice_verified=false\nhardware_written=false\n", stdout);
        else {
            fputs(",\"status\":", stdout); print_status(&fan, &state, true);
            fprintf(stdout, ",\"settled\":%s}", settled ? "true" : "false");
        }
    }
    if (cli_json) fputs("}\n", stdout);
    return flush_output(stdout);
}

int main(int argc, char **argv)
{
    setup("y720-fan", argc, argv);
    int result = run(argc, argv);
    return result < 0 ? report_error() : result;
}
