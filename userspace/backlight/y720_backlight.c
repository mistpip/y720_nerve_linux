/* SPDX-License-Identifier: MIT */
/* One-shot I2C hidraw client; no kernel RGB policy or remembered device state. */
#include "../common/cli.h"
#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>

static const unsigned char descriptor[] = {
    0x06,0x89,0xff,0x09,0xcc,0xa1,0x01,0x85,0xcc,0x09,0x01,0x15,0x00,0x26,0xff,0x00,
    0x75,0x03,0x95,0x10,0xb1,0x00,0xc0,0x06,0x83,0xff,0x09,0x80,0xa1,0x01,0x85,0x5a,
    0x09,0x01,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x10,0xb1,0x00,0xc0
};
static const char descriptor_sha[] = "587474323e725222dfa27cb66742b145b0f244e089cf2649f7d6bc0f86f50281";
static const char *const effects[] = {"heartbeat", "breath", "smooth", "static", "wave"};
struct named_color { const char *name; int value; };
static const struct named_color colors[] = {
    {"red",0},{"torch-red",1},{"pink",2},{"magenta",3},{"purple",4},{"blue",6},
    {"cyan",9},{"green",12},{"yellow",15},{"orange",16},{"white",18},{"none",19}
};
struct profile { int effect, color[4], brightness[4]; };
struct backlight_device { char name[64], path[PATH_MAX], hid_name[PATH_MAX]; };

static int color_value(const char *text)
{
    for (size_t i = 0; i < ARRAY_SIZE(colors); ++i)
        if (!strcmp(text, colors[i].name)) return colors[i].value;
    size_t length = strlen(text);
    if (length < 1 || length > 2) return -1;
    for (size_t i = 0; i < length; ++i) if (text[i] < '0' || text[i] > '9') return -1;
    int value = atoi(text); /* Length and decimal digits bounded above. */
    return value <= 19 ? value : -1;
}

static int effect_value(const char *text)
{
    for (size_t i = 0; i < ARRAY_SIZE(effects); ++i) if (!strcmp(text, effects[i])) return (int)i;
    return -1;
}

/* Schema-specific, bounded JSON reader: no recursion, allocations, or unknown values.
 * All valid schema strings are ASCII; escaped ASCII is accepted, other Unicode
 * cannot name a field/effect/color and is rejected rather than truncated. */
struct json_reader { const unsigned char *at, *end; };
static void json_space(struct json_reader *reader)
{
    while (reader->at < reader->end && (*reader->at == ' ' || *reader->at == '\t' || *reader->at == '\n' || *reader->at == '\r')) ++reader->at;
}
static bool json_take(struct json_reader *reader, unsigned char ch)
{
    json_space(reader);
    if (reader->at == reader->end || *reader->at != ch) return false;
    ++reader->at; return true;
}
static int json_string(struct json_reader *reader, char *out, size_t capacity)
{
    if (!json_take(reader, '"')) return -1;
    size_t used = 0;
    while (reader->at < reader->end) {
        unsigned ch = *reader->at++;
        if (ch == '"') { out[used] = 0; return 0; }
        if (ch == '\\') {
            if (reader->at == reader->end) return -1;
            ch = *reader->at++;
            switch (ch) {
            case '"': case '\\': case '/': break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case 'u':
                ch = 0;
                for (int i = 0; i < 4; ++i) {
                    if (reader->at == reader->end) return -1;
                    unsigned digit = *reader->at++;
                    if (digit >= '0' && digit <= '9') digit -= '0';
                    else if (digit >= 'a' && digit <= 'f') digit -= 'a' - 10;
                    else if (digit >= 'A' && digit <= 'F') digit -= 'A' - 10;
                    else return -1;
                    ch = ch * 16 + digit;
                }
                if (!ch || ch > 127) return -1;
                break;
            default: return -1;
            }
        } else if (ch < 32 || ch > 127) return -1;
        if (!ch || used + 1 >= capacity) return -1;
        out[used++] = (char)ch;
    }
    return -1;
}
static int json_integer(struct json_reader *reader, int low, int high, int *value)
{
    json_space(reader);
    bool negative = reader->at < reader->end && *reader->at == '-';
    if (negative) ++reader->at;
    if (reader->at == reader->end || *reader->at < '0' || *reader->at > '9') return -1;
    bool zero = *reader->at == '0';
    int number = *reader->at++ - '0';
    while (reader->at < reader->end && *reader->at >= '0' && *reader->at <= '9') {
        if (zero || number > 10000) return -1;
        number = number * 10 + (*reader->at++ - '0');
    }
    if (negative) number = -number;
    if (number < low || number > high) return -1;
    *value = number;
    return 0;
}
static int parse_profile(const unsigned char *raw, size_t length, struct profile *profile)
{
    struct json_reader reader = {.at = raw, .end = raw + length};
    unsigned fields = 0;
    if (length > 4096) goto invalid;
    /* Retain the reference reader's acceptance of an optional UTF-8 BOM. */
    if (length >= 3 && !memcmp(raw, "\xef\xbb\xbf", 3)) reader.at += 3;
    if (!json_take(&reader, '{')) goto invalid;
    for (;;) {
        char key[64], text[64];
        if (json_string(&reader, key, sizeof(key)) || !json_take(&reader, ':')) goto invalid;
        unsigned bit;
        if (!strcmp(key, "schema_version")) bit = 1;
        else if (!strcmp(key, "effect")) bit = 2;
        else if (!strcmp(key, "colors")) bit = 4;
        else if (!strcmp(key, "brightness")) bit = 8;
        else goto invalid;
        if (fields & bit) goto invalid;
        fields |= bit;
        if (bit == 1) {
            int version;
            if (json_integer(&reader, 1, 1, &version)) goto invalid;
        } else if (bit == 2) {
            if (json_string(&reader, text, sizeof(text)) || (profile->effect = effect_value(text)) < 0) goto invalid;
        } else {
            if (!json_take(&reader, '[')) goto invalid;
            for (size_t i = 0; i < 4; ++i) {
                json_space(&reader);
                if (bit == 4 && reader.at < reader.end && *reader.at == '"') {
                    if (json_string(&reader, text, sizeof(text)) || (profile->color[i] = color_value(text)) < 0) goto invalid;
                } else if (json_integer(&reader, 0, bit == 4 ? 19 : 3,
                                        bit == 4 ? &profile->color[i] : &profile->brightness[i])) goto invalid;
                if (i < 3 && !json_take(&reader, ',')) goto invalid;
            }
            if (!json_take(&reader, ']')) goto invalid;
        }
        if (json_take(&reader, '}')) break;
        if (!json_take(&reader, ',')) goto invalid;
    }
    json_space(&reader);
    if (fields != 15 || reader.at != reader.end) goto invalid;
    return 0;
invalid:
    return fail("usage", 2, "Expected UTF-8 JSON profile schema 1 with exactly effect/colors/brightness/schema_version");
}

static int load_profile(const char *filename, struct profile *profile)
{
    int fd = !strcmp(filename, "-") ? STDIN_FILENO : open(filename, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return io_error("open profile");
    unsigned char bytes[4097];
    size_t used = 0;
    while (used < sizeof(bytes)) {
        ssize_t count = read(fd, bytes + used, sizeof(bytes) - used);
        if (count < 0) {
            int saved = errno;
            if (fd != STDIN_FILENO) close(fd);
            errno = saved; return io_error("read profile");
        }
        if (!count) break;
        used += (size_t)count;
        if (interrupted()) { if (fd != STDIN_FILENO) close(fd); return -1; }
    }
    if (fd != STDIN_FILENO) close(fd);
    return parse_profile(bytes, used, profile);
}

static void packets(const struct profile *profile, unsigned char reports[5][7])
{
    memset(reports, 0, 35);
    for (size_t i = 0; i < 4; ++i) {
        reports[i][0] = 0xcc;
        reports[i][2] = (unsigned char)profile->effect;
        reports[i][3] = (unsigned char)profile->color[i];
        reports[i][4] = (unsigned char)profile->brightness[i];
        reports[i][5] = (unsigned char)i;
    }
    reports[4][0] = 0xcc; reports[4][1] = 9;
}

static int backlight_validate(const struct backlight_device *device)
{
    char path[PATH_MAX], resolved[PATH_MAX];
    unsigned char bytes[sizeof(descriptor)];
    if (format_path(path, "%s/device/driver", device->path)) return -1;
    if (!realpath(path, resolved)) return io_error("resolve HID driver");
    if (strcmp(strrchr(resolved, '/') + 1, "hid-generic")) return fail("driver_conflict", 3, "Backlight must use hid-generic; no RGB-driver bypass");
    if (format_path(path, "%s/device/report_descriptor", device->path)) return -1;
    ssize_t size = read_bounded(path, bytes, sizeof(bytes));
    if (size < 0) {
        if (cli_error.code && !strcmp(cli_error.code, "invalid_readback")) return fail("unsupported_descriptor", 3, "Unverified report descriptor size");
        return -1;
    }
    if (size != sizeof(bytes) || memcmp(bytes, descriptor, sizeof(bytes))) return fail("unsupported_descriptor", 3, "Unverified backlight report descriptor");
    return 0;
}

static int backlight_discover(struct backlight_device *device, const char *selected)
{
#if !defined(__linux__) || !defined(__x86_64__)
    return fail("unsupported_device", 3, "Only x86_64 Linux is verified");
#endif
    if (verify_dmi()) return -1;
    DIR *directory = opendir("/sys/class/hidraw");
    if (!directory) return errno == ENOENT ? fail("device_not_found", 3, "No hidraw directory") : io_error("list hidraw");
    int matches = 0, result = -1;
    struct dirent *entry;
    for (;;) {
        errno = 0;
        entry = readdir(directory);
        if (!entry) { if (errno) { io_error("enumerate hidraw"); goto done; } break; }
        if (!numbered_name(entry->d_name, "hidraw") || (selected && strcmp(selected, entry->d_name))) continue;
        char path[PATH_MAX], uevent[4096], resolved[PATH_MAX];
        if (format_path(path, "/sys/class/hidraw/%s/device/uevent", entry->d_name)) goto done;
        if (read_text(path, uevent, sizeof(uevent))) {
            if (cli_error.errnum == ENOENT) { cli_error = (struct cli_error){.completed = -1}; continue; }
            goto done;
        }
        bool id_ok = false, name_ok = false;
        char *context;
        for (char *line = strtok_r(uevent, "\n", &context); line; line = strtok_r(NULL, "\n", &context)) {
            if (!strncmp(line, "HID_ID=", 7)) {
                unsigned bus, vendor, product; char extra;
                id_ok = sscanf(line + 7, "%x:%x:%x%c", &bus, &vendor, &product, &extra) == 3 && bus == 0x18 && vendor == 0x048d && product == 0x837a;
            }
            if (!strncmp(line, "HID_NAME=", 9)) name_ok = !strncmp(line + 9, "ITE33D1:", 8);
        }
        if (!id_ok || !name_ok) continue;
        strcpy(device->name, entry->d_name);
        if (format_path(device->path, "/sys/class/hidraw/%s", entry->d_name) || backlight_validate(device)) goto done;
        if (format_path(path, "%s/device", device->path)) goto done;
        if (!realpath(path, resolved)) { io_error("resolve HID device"); goto done; }
        strcpy(device->hid_name, strrchr(resolved, '/') + 1);
        ++matches;
    }
    result = matches == 1 ? 0 : fail("device_not_found", 3, "Expected one verified I2C backlight, found %d", matches);
done:
    closedir(directory);
    return result;
}

static int opened_hid(const struct backlight_device *device)
{
    if (backlight_validate(device) || interrupted()) return -1;
    char path[PATH_MAX], text[96], extra;
    unsigned expected_major, expected_minor;
    if (format_path(path, "%s/dev", device->path) || read_text(path, text, sizeof(text))) return -1;
    if (sscanf(text, "%u:%u%c", &expected_major, &expected_minor, &extra) != 2) return fail("invalid_readback", 6, "Invalid hidraw device number");
    if (format_path(path, "/dev/%s", device->name)) return -1;
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return io_error("open hidraw");
    struct stat info;
    if (fstat(fd, &info)) { io_error("stat hidraw"); goto failed; }
    if (!S_ISCHR(info.st_mode) || major(info.st_rdev) != expected_major || minor(info.st_rdev) != expected_minor) {
        fail("identity_changed", 3, "Opened node differs from verified sysfs device"); goto failed;
    }
    if (flock(fd, LOCK_EX | LOCK_NB)) { io_error("lock hidraw"); goto failed; }
    struct hidraw_devinfo identity = {0};
    int size = 0;
    struct hidraw_report_descriptor report = {.size = sizeof(descriptor)};
    if (ioctl(fd, HIDIOCGRAWINFO, &identity) < 0 || ioctl(fd, HIDIOCGRDESCSIZE, &size) < 0) { io_error("read HID identity"); goto failed; }
    if (identity.bustype != 0x18 || (uint16_t)identity.vendor != 0x048d || (uint16_t)identity.product != 0x837a || size != (int)sizeof(descriptor)) {
        fail("identity_changed", 3, "Opened HID identity or descriptor size changed"); goto failed;
    }
    if (ioctl(fd, HIDIOCGRDESC, &report) < 0) { io_error("read HID descriptor"); goto failed; }
    if (report.size != sizeof(descriptor) || memcmp(report.value, descriptor, sizeof(descriptor))) {
        fail("identity_changed", 3, "Opened HID descriptor changed"); goto failed;
    }
    if (backlight_validate(device) || interrupted()) goto failed;
    return fd;
failed:
    close(fd); return -1;
}

static int transfer(int fd, const struct profile *profile)
{
    unsigned char reports[5][7];
    packets(profile, reports);
    cli_error.completed = 0;
    for (size_t i = 0; i < 5; ++i) {
        if (interrupted()) return -1;
        int count = ioctl(fd, HIDIOCSFEATURE(7), reports[i]);
        if (count != 7) {
            cli_error.errnum = count < 0 ? errno : EIO;
            cli_error.uncertain = true;
            if (cli_signal) return interrupted();
            return fail("write_uncertain", 7, "Feature transfer failed/short; no retry or rollback");
        }
        ++cli_error.completed;
        if (interrupted()) return -1;
    }
    return 0;
}

static void print_profile(const struct profile *profile)
{
    fputs("{\"schema_version\":1,\"effect\":", stdout); quote(stdout, effects[profile->effect]);
    fprintf(stdout, ",\"colors\":[%d,%d,%d,%d],\"brightness\":[%d,%d,%d,%d]}",
            profile->color[0],profile->color[1],profile->color[2],profile->color[3],
            profile->brightness[0],profile->brightness[1],profile->brightness[2],profile->brightness[3]);
}
static void print_info(const struct backlight_device *device)
{
    const char *separator = cli_json ? "," : "\n";
    fputs(cli_json ? "{\"device\":" : "device=", stdout); quote(stdout, device->name);
    fputs(separator, stdout); fputs(cli_json ? "\"hid_device\":" : "hid_device=", stdout); quote(stdout, device->hid_name);
    fprintf(stdout, "%s%s\"hid-generic\"%s%s\"%s\"%s%s7%s%sfalse%s%snull%s", separator,
            cli_json ? "\"driver\":" : "driver=", separator,
            cli_json ? "\"descriptor_sha256\":" : "descriptor_sha256=", descriptor_sha, separator,
            cli_json ? "\"report_bytes\":" : "report_bytes=", separator,
            cli_json ? "\"state_known\":" : "state_known=", separator,
            cli_json ? "\"profile\":" : "profile=", cli_json ? "}" : "\n");
}
static void print_result(const struct profile *profile, bool dry_run)
{
    fputs(cli_json ? "{\"profile\":" : "profile=", stdout); print_profile(profile);
    if (dry_run) {
        unsigned char reports[5][7]; packets(profile, reports);
        fputs(cli_json ? ",\"reports_hex\":[" : "\nreports_hex=[", stdout);
        for (size_t i = 0; i < 5; ++i) {
            if (i) fputc(',', stdout);
            fputc('"', stdout);
            for (size_t j = 0; j < 7; ++j) fprintf(stdout, "%s%02x", j ? " " : "", reports[i][j]);
            fputc('"', stdout);
        }
        fputs(cli_json ? "],\"device_verified\":false,\"hardware_written\":false}" : "]\ndevice_verified=false\nhardware_written=false\n", stdout);
    } else fputs(",\"reports_completed\":5,\"transport_accepted\":true,\"visual_verified\":false,\"state_known\":false}", stdout);
}
static void help(void)
{
    puts("y720-backlight " CLI_VERSION " — userspace hidraw client\n"
         "Usage: y720-backlight [--json] [--dry-run] [--device hidrawN] COMMAND\n"
         "  status (info is an alias)\n  off\n  apply FILE|-\n"
         "  set [--effect heartbeat|breath|smooth|static|wave]\n"
         "      (--color COLOR | --colors C C C C) [--brightness 0..3 | --levels L L L L]\n"
         "Colors: red, torch-red, pink, magenta, purple, blue, cyan, green, yellow,\n"
         "orange, white, none; or indices 0..19. Default: static, brightness 3.\n"
         "No reliable device-state readback. Writes silent unless --json/--dry-run.");
}

static int run(int argc, char **argv)
{
    struct global_options options = {0};
    struct profile profile = {.effect = 3, .brightness = {3,3,3,3}};
    for (int j = 1; j < argc; ++j) {
        if (!strcmp(argv[j], "--help") || !strcmp(argv[j], "-h")) { help(); return flush_output(stdout); }
        if (!strcmp(argv[j], "--version")) { puts("y720-backlight " CLI_VERSION); return flush_output(stdout); }
    }
    if (globals(argc, argv, &options)) return -1;
    if (options.device && !numbered_name(options.device, "hidraw")) return fail("usage", 2, "--device must be a hidrawN name");
    int i = options.command_index;
    if (i >= argc) return fail("usage", 2, "A command is required; see --help");
    const char *command = argv[i++];
    bool query = !strcmp(command, "status") || !strcmp(command, "info");
    if (!strcmp(command, "apply")) {
        if (i + 1 != argc) return fail("usage", 2, "apply requires one file or -");
        if (load_profile(argv[i++], &profile)) return -1;
    } else if (!strcmp(command, "off")) memset(profile.brightness, 0, sizeof(profile.brightness));
    else if (!strcmp(command, "set")) {
        bool color_set = false, level_set = false;
        for (; i < argc; ++i) {
            char option[32];
            const char *equal = strchr(argv[i], '=');
            size_t length = equal ? (size_t)(equal - argv[i]) : strlen(argv[i]);
            if (length >= sizeof(option)) return fail("usage", 2, "Unknown set option");
            memcpy(option, argv[i], length); option[length] = 0;
            bool color = !strcmp(option, "--color"), multi_color = !strcmp(option, "--colors");
            bool level = !strcmp(option, "--brightness"), multi_level = !strcmp(option, "--levels");
            bool effect = !strcmp(option, "--effect");
            if (!color && !multi_color && !level && !multi_level && !effect) return fail("usage", 2, "Unknown set option");
            if (((color || multi_color) && color_set) || ((level || multi_level) && level_set)) return fail("usage", 2, "Conflicting profile options");
            int count = multi_color || multi_level ? 4 : 1;
            if (equal && count == 4) return fail("usage", 2, "Four-value options require four separate arguments");
            for (int j = 0; j < count; ++j) {
                const char *value = equal ? equal + 1 : ++i < argc ? argv[i] : NULL;
                if (!value) return fail("usage", 2, "Option requires a value");
                if (effect) {
                    profile.effect = effect_value(value);
                    if (profile.effect < 0) return fail("usage", 2, "Unknown effect");
                } else if (color || multi_color) {
                    int parsed = color_value(value);
                    if (parsed < 0) return fail("usage", 2, "Color must be a known name or index 0..19");
                    if (color) for (size_t k = 0; k < 4; ++k) profile.color[k] = parsed;
                    else profile.color[j] = parsed;
                } else {
                    long long parsed;
                    if (parse_integer(value, 0, 3, &parsed)) return fail("usage", 2, "Brightness must be integer 0..3");
                    if (level) for (size_t k = 0; k < 4; ++k) profile.brightness[k] = (int)parsed;
                    else profile.brightness[j] = (int)parsed;
                }
            }
            color_set |= color || multi_color;
            level_set |= level || multi_level;
        }
        if (!color_set) return fail("usage", 2, "Specify --color or --colors");
    } else if (!query) return fail("usage", 2, "Unknown command");
    if (i != argc || (options.dry_run && query)) return fail("usage", 2, "Unexpected arguments or read-only dry-run");
    if (interrupted()) return -1;
    struct backlight_device device = {0};
    if (!options.dry_run) {
        if (backlight_discover(&device, options.device)) return -1;
        if (!query) {
            int lock = writer_lock("/run/y720-backlight.lock");
            if (lock < 0) return -1;
            int fd = opened_hid(&device);
            if (fd < 0) { close(lock); return -1; }
            int result = transfer(fd, &profile);
            /* close() on hidraw does not roll back accepted Feature reports. */
            if (close(fd) && !result) {
                io_error("close hidraw"); cli_error.uncertain = true;
                if (!cli_signal) fail("write_uncertain", 7, "hidraw close failed after Feature transfers");
                result = -1;
            }
            close(lock);
            if (result) return -1;
        }
    }
    if (interrupted()) return -1;
    if (!cli_json && !options.dry_run && !query) return 0;
    if (cli_json) envelope(stdout, true);
    if (query) print_info(&device); else print_result(&profile, options.dry_run);
    if (cli_json) fputs("}\n", stdout);
    return flush_output(stdout);
}

int main(int argc, char **argv)
{
    setup("y720-backlight", argc, argv);
    int result = run(argc, argv);
    return result < 0 ? report_error() : result;
}
