/* SPDX-License-Identifier: MIT */
/* Shared, compiled-in plumbing only. No fan/backlight policy or runtime library. */
#ifndef Y720_CLI_H
#define Y720_CLI_H
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CLI_VERSION "0.1"
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
struct cli_error {
    const char *code;
    char message[512];
    int status, errnum, completed;
    bool uncertain;
    const char *phase;
};
static struct cli_error cli_error = {.completed = -1};
static volatile sig_atomic_t cli_signal;
static bool cli_json;
static const char *cli_program;

static inline int fail(const char *code, int status, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
static inline int fail(const char *code, int status, const char *format, ...)
{
    va_list ap;
    cli_error.code = code;
    cli_error.status = status;
    va_start(ap, format);
    vsnprintf(cli_error.message, sizeof(cli_error.message), format, ap);
    va_end(ap);
    return -1;
}

static int interrupted(void)
{
    if (!cli_signal)
        return 0;
    cli_error.uncertain = true;
    return fail("interrupted", 128 + (int)cli_signal,
                "Cancelled; no implicit hardware rollback");
}

static int io_error(const char *operation)
{
    int saved = errno;
    if (cli_signal)
        return interrupted();
    cli_error.errnum = saved;
    return fail(saved == EACCES || saved == EPERM ? "permission" :
                saved == EAGAIN || saved == EWOULDBLOCK ? "busy" : "io_error",
                saved == EACCES || saved == EPERM ? 4 :
                saved == EAGAIN || saved == EWOULDBLOCK ? 5 : 6,
                "%s: %s", operation, strerror(saved));
}

static void signal_handler(int sig)
{
    cli_signal = sig;
}

static void setup(const char *program, int argc, char **argv)
{
    struct sigaction action = {0};
    cli_program = program;
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--json"))
            cli_json = true;
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
}

static void quote(FILE *out, const char *value)
{
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        switch (*p) {
        case '"': fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        default:
            if (*p < 32)
                fprintf(out, "\\u%04x", (unsigned)*p);
            else
                fputc(*p, out);
        }
    }
    fputc('"', out);
}

static void envelope(FILE *out, bool ok)
{
    fputs("{\"schema_version\":1,\"program\":", out);
    quote(out, cli_program);
    fprintf(out, ",\"version\":\"%s\",\"ok\":%s,\"%s\":",
            CLI_VERSION, ok ? "true" : "false", ok ? "data" : "error");
}

static int flush_output(FILE *out)
{
    if (fflush(out) == EOF || ferror(out))
        return errno == EPIPE ? 141 : 6;
    return 0;
}

static int report_error(void)
{
    if (!cli_error.status)
        fail("io_error", 6, "Unspecified operation failure");
    if (cli_json) {
        bool comma = false;
        envelope(stderr, false);
        fputs("{\"code\":", stderr);
        quote(stderr, cli_error.code ? cli_error.code : "io_error");
        fputs(",\"message\":", stderr);
        quote(stderr, cli_error.message);
        fputs(",\"details\":{", stderr);
        if (cli_error.uncertain) {
            fputs("\"state_uncertain\":true", stderr);
            comma = true;
        }
        if (cli_error.phase) {
            fprintf(stderr, "%s\"phase\":", comma ? "," : "");
            quote(stderr, cli_error.phase);
            comma = true;
        }
        if (cli_error.completed >= 0) {
            fprintf(stderr, "%s\"reports_completed\":%d", comma ? "," : "", cli_error.completed);
            comma = true;
        }
        if (cli_error.errnum)
            fprintf(stderr, "%s\"errno\":%d", comma ? "," : "", cli_error.errnum);
        fputs("}}}\n", stderr);
    } else {
        fprintf(stderr, "%s: %s: %s\n", cli_program, cli_error.code, cli_error.message);
    }
    int status = flush_output(stderr);
    return status ? status : cli_error.status;
}

static inline int format_path(char *out, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
static inline int format_path(char *out, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int size = vsnprintf(out, PATH_MAX, format, ap);
    va_end(ap);
    if (size < 0 || size >= PATH_MAX)
        return fail("io_error", 6, "Path exceeds supported size");
    return 0;
}

/* Returns size, -1 on error; never silently truncates data. */
static ssize_t read_bounded(const char *path, void *buffer, size_t limit)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return io_error("open for read");
    size_t used = 0;
    unsigned char *bytes = buffer;
    while (used < limit) {
        ssize_t count = read(fd, bytes + used, limit - used);
        if (count < 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return io_error("read");
        }
        if (!count)
            break;
        used += (size_t)count;
        if (interrupted()) {
            close(fd);
            return -1;
        }
    }
    unsigned char extra;
    ssize_t more = read(fd, &extra, 1);
    int saved = errno;
    close(fd);
    if (more < 0) {
        errno = saved;
        return io_error("read");
    }
    if (more > 0)
        return fail("invalid_readback", 6, "Input exceeds supported size");
    return (ssize_t)used;
}

static int read_text(const char *path, char *buffer, size_t capacity)
{
    ssize_t count = read_bounded(path, buffer, capacity - 1);
    if (count < 0)
        return -1;
    if (memchr(buffer, 0, (size_t)count))
        return fail("invalid_readback", 6, "Unexpected NUL in text input");
    buffer[count] = 0;
    while (count > 0 && isspace((unsigned char)buffer[count - 1]))
        buffer[--count] = 0;
    return 0;
}

static int parse_integer(const char *text, long long low, long long high,
                         long long *result)
{
    char *end;
    errno = 0;
    long long value = strtoll(text, &end, 10);
    if (end == text || errno == ERANGE)
        return -1;
    while (isspace((unsigned char)*end))
        ++end;
    if (*end || value < low || value > high)
        return -1;
    *result = value;
    return 0;
}

static inline int read_integer(const char *path, long long *value)
{
    char text[96];
    if (read_text(path, text, sizeof(text)))
        return -1;
    if (parse_integer(text, LLONG_MIN, LLONG_MAX, value))
        return fail("invalid_readback", 6, "Non-integer sysfs value");
    return 0;
}

static inline int verify_dmi(void)
{
    static const char *const keys[] = {"sys_vendor", "product_name", "product_version", "bios_version"};
    static const char *const values[] = {"LENOVO", "80Y4", "Y720-15IKB", "4GCN38WW"};
    for (size_t i = 0; i < ARRAY_SIZE(keys); ++i) {
        char path[PATH_MAX], text[256];
        if (format_path(path, "/sys/class/dmi/id/%s", keys[i]))
            return -1;
        if (read_text(path, text, sizeof(text))) {
            if (cli_error.errnum == ENOENT)
                return fail("unsupported_device", 3, "Required DMI identity is unavailable");
            return -1;
        }
        if (strcmp(text, values[i]))
            return fail("unsupported_device", 3, "Unverified DMI %s", keys[i]);
    }
    return 0;
}

static bool numbered_name(const char *name, const char *prefix)
{
    size_t length = strlen(prefix);
    if (strncmp(name, prefix, length) || !name[length])
        return false;
    for (const unsigned char *p = (const unsigned char *)name + length; *p; ++p)
        if (*p < '0' || *p > '9')
            return false;
    return strlen(name) < 64;
}

static inline int writer_lock(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return io_error("open writer lock");
    struct stat info;
    if (fstat(fd, &info)) {
        int saved = errno;
        close(fd);
        errno = saved;
        return io_error("stat writer lock");
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1 || (info.st_mode & 077)) {
        close(fd);
        return fail("unsafe_lock", 5, "Unsafe writer lock ownership/type/permissions");
    }
    if (flock(fd, LOCK_EX | LOCK_NB)) {
        int saved = errno;
        close(fd);
        if (saved == EAGAIN || saved == EWOULDBLOCK)
            return fail("busy", 5, "Another cooperating writer is active");
        errno = saved;
        return io_error("lock writer");
    }
    return fd;
}

static inline double monotonic_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return -1;
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static inline int sleep_seconds(double seconds)
{
    struct timespec duration = {.tv_sec = (time_t)seconds,
        .tv_nsec = (long)((seconds - floor(seconds)) * 1000000000.0)};
    if (interrupted())
        return -1;
    if (nanosleep(&duration, NULL))
        return io_error("wait");
    return interrupted();
}

struct global_options {
    bool dry_run;
    const char *device;
    int command_index;
};

/* Command-specific options are parsed by each separate program. */
static int globals(int argc, char **argv, struct global_options *options)
{
    int i = 1;
    for (; i < argc && !strncmp(argv[i], "--", 2); ++i) {
        if (!strcmp(argv[i], "--json"))
            cli_json = true;
        else if (!strcmp(argv[i], "--dry-run"))
            options->dry_run = true;
        else if (!strcmp(argv[i], "--device")) {
            if (++i >= argc)
                return fail("usage", 2, "--device requires a name");
            options->device = argv[i];
        } else if (!strncmp(argv[i], "--device=", 9)) {
            options->device = argv[i] + 9;
        } else {
            return fail("usage", 2, "Unknown global option");
        }
    }
    options->command_index = i;
    return 0;
}

#endif
