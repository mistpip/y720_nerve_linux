/* SPDX-License-Identifier: MIT */
/* Post-open hidraw identity checks, all syscalls replaced at compile time. */
#include "../common/cli.h"
#include <assert.h>
#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
static int mock_open(const char *, int, ...);
static int mock_close(int);
static int mock_fstat(int, struct stat *);
static int mock_flock(int, int);
static int mock_ioctl(int, unsigned long, ...);
static char *mock_realpath(const char *, char *);
static ssize_t mock_read_bounded(const char *, void *, size_t);
static int mock_read_text(const char *, char *, size_t);
static int mock_verify_dmi(void);
static DIR *mock_opendir(const char *);
static struct dirent *mock_readdir(DIR *);
static int mock_closedir(DIR *);
static int mock_writer_lock(const char *);
#define open mock_open
#define close mock_close
#define fstat mock_fstat
#define flock mock_flock
#define ioctl mock_ioctl
#define realpath mock_realpath
#define read_bounded mock_read_bounded
#define read_text mock_read_text
#define verify_dmi mock_verify_dmi
#define opendir mock_opendir
#define readdir mock_readdir
#define closedir mock_closedir
#define writer_lock mock_writer_lock
#define main backlight_program_main
#include "../backlight/y720_backlight.c"
#undef main
static int fault, closes, queries, feature_writes, query_only, directory_entries;
static int directory_marker;
static int mock_verify_dmi(void) { return 0; }
static DIR *mock_opendir(const char *path)
{
    assert(query_only && !strcmp(path, "/sys/class/hidraw"));
    directory_entries = 0;
    return (DIR *)&directory_marker;
}
static struct dirent *mock_readdir(DIR *directory)
{
    assert(directory == (DIR *)&directory_marker);
    static struct dirent entry = {.d_name = "hidraw9"};
    if (directory_entries++) return NULL;
    return &entry;
}
static int mock_closedir(DIR *directory) { assert(directory == (DIR *)&directory_marker); return 0; }
static int mock_writer_lock(const char *path) { (void)path; assert(!"Status must never take writer lock"); return -1; }
static char *mock_realpath(const char *path, char *resolved)
{
    if (query_only && !strstr(path, "/device/driver")) {
        strcpy(resolved, "/mock/0018:048D:837A.0001"); return resolved;
    }
    assert(strstr(path, "/device/driver"));
    strcpy(resolved, fault == 1 ? "/mock/rgb-driver" : "/mock/hid-generic");
    return resolved;
}
static ssize_t mock_read_bounded(const char *path, void *buffer, size_t limit)
{
    assert(strstr(path, "report_descriptor") && limit == sizeof(descriptor));
    memcpy(buffer, descriptor, sizeof(descriptor));
    if (fault == 2) ((unsigned char *)buffer)[0] ^= 1;
    return fault == 3 ? 45 : 46;
}
static int mock_read_text(const char *path, char *text, size_t capacity)
{
    if (query_only) {
        assert(strstr(path, "/device/uevent"));
        snprintf(text, capacity, "HID_ID=0018:0000048D:0000837A\nHID_NAME=ITE33D1:00 048D:837A\n");
    } else { assert(strstr(path, "/dev")); snprintf(text, capacity, "10:44"); }
    return 0;
}
static int mock_open(const char *path, int flags, ...)
{
    assert(!query_only && !strcmp(path, "/dev/hidraw9") && (flags & O_NOFOLLOW) && (flags & O_CLOEXEC));
    if (fault == 4) { errno = EACCES; return -1; }
    return 123;
}
static int mock_close(int fd) { assert(fd == 123); ++closes; return 0; }
static int mock_fstat(int fd, struct stat *info)
{
    assert(fd == 123);
    *info = (struct stat){.st_mode = fault == 5 ? S_IFREG : S_IFCHR, .st_rdev = makedev(10, fault == 6 ? 45 : 44)};
    return 0;
}
static int mock_flock(int fd, int flags)
{
    assert(fd == 123 && flags == (LOCK_EX | LOCK_NB));
    if (fault == 7) { errno = EAGAIN; return -1; }
    return 0;
}
static int mock_ioctl(int fd, unsigned long command, ...)
{
    assert(fd == 123); ++queries;
    va_list args; va_start(args, command); void *data = va_arg(args, void *); va_end(args);
    if (fault == 8) { errno = EIO; return -1; }
    if (command == HIDIOCGRAWINFO) {
        struct hidraw_devinfo *identity = data;
        *identity = (struct hidraw_devinfo){.bustype = 0x18, .vendor = 0x048d, .product = (short)0x837a};
        if (fault == 9) identity->bustype = 3;
        if (fault == 10) identity->vendor = 0;
        if (fault == 11) identity->product = 0;
    } else if (command == HIDIOCGRDESCSIZE) *(int *)data = fault == 12 ? 45 : 46;
    else if (command == HIDIOCGRDESC) {
        struct hidraw_report_descriptor *report = data;
        memcpy(report->value, descriptor, sizeof(descriptor));
        if (fault == 13) report->value[0] ^= 1;
        if (fault == 14) report->size = 45;
        if (fault == 15) fault = 1; /* Driver changed between open and validation. */
    } else { ++feature_writes; assert(!"Unexpected Feature command during identity validation"); }
    return 0;
}
int main(int argc, char **argv)
{
    if (argc > 1) {
        query_only = 1;
        setup("y720-backlight", argc, argv);
        int result = run(argc, argv);
        assert(!queries && !feature_writes && !closes);
        return result < 0 ? report_error() : result;
    }
    struct backlight_device device = {.name = "hidraw9", .path = "/mock/hidraw9"};
    for (int scenario = 0; scenario <= 15; ++scenario) {
        fault = scenario; closes = queries = feature_writes = 0;
        cli_error = (struct cli_error){.completed = -1};
        int fd = opened_hid(&device);
        if (!scenario) { assert(fd == 123 && queries == 3 && !closes); mock_close(fd); }
        else {
            assert(fd < 0);
            assert(closes == (scenario >= 5 ? 1 : 0));
            assert(cli_error.status == (scenario == 4 ? 4 : scenario == 7 ? 5 : scenario == 8 ? 6 : 3));
        }
        assert(!feature_writes);
    }
    puts("PASS C HID identity: signed PID bit pattern, wrong driver/descriptor/node/bus/vendor/product/length/lock and post-open identity change; zero Feature writes");
    return 0;
}
