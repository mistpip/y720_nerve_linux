/* SPDX-License-Identifier: MIT */
/* Pure parser/packet and mocked ioctl tests. Never opens a device. */
#include "../common/cli.h"
#include <assert.h>
#include <linux/hidraw.h>
#include <sys/ioctl.h>
static int mock_ioctl(int, unsigned long, ...);
#define ioctl mock_ioctl
#define main backlight_program_main
#include "../backlight/y720_backlight.c"
#undef main
static int calls, failure_position, failure_kind;
static int mock_ioctl(int fd, unsigned long command, ...)
{
    assert(fd == 123 && command == HIDIOCSFEATURE(7));
    va_list args; va_start(args, command);
    unsigned char *packet = va_arg(args, unsigned char *);
    assert(packet[0] == 0xcc && (calls == 4 ? packet[1] == 9 : packet[5] == calls));
    va_end(args);
    if (calls++ == failure_position) {
        if (failure_kind == 0) { errno = EIO; return -1; }
        if (failure_kind == 1) return 3;
        cli_signal = failure_kind == 2 ? SIGTERM : SIGINT; errno = EINTR; return -1;
    }
    return 7;
}
int main(void)
{
    struct profile profile;
    unsigned char report[5][7];
    for (int effect = 0; effect < 5; ++effect) for (int color = 0; color < 20; ++color) for (int level = 0; level < 4; ++level) {
        profile = (struct profile){.effect = effect, .color = {color,color,color,color}, .brightness = {level,level,level,level}};
        packets(&profile, report);
        for (int zone = 0; zone < 4; ++zone) assert(report[zone][0] == 0xcc && report[zone][1] == 0 && report[zone][2] == effect && report[zone][3] == color && report[zone][4] == level && report[zone][5] == zone && !report[zone][6]);
        assert(!memcmp(report[4], (unsigned char[]){0xcc,9,0,0,0,0,0}, 7));
    }
    profile = (struct profile){.effect = 3};
    failure_position = -1; calls = 0;
    assert(!transfer(123, &profile) && calls == 5);
    for (int position = 0; position < 5; ++position) for (int kind = 0; kind < 4; ++kind) {
        calls = 0; failure_position = position; failure_kind = kind;
        cli_signal = 0; cli_error = (struct cli_error){.completed = -1};
        assert(transfer(123, &profile) == -1 && calls == position + 1 && cli_error.completed == position && cli_error.uncertain);
        assert(cli_error.status == (kind < 2 ? 7 : kind == 2 ? 143 : 130));
    }
    cli_signal = 0;
    /* Deterministic bounded parser fuzzing under ASan/UBSan, not hardware fuzzing. */
    uint32_t random_value = 123;
    unsigned char bytes[4097];
    static const unsigned char seed[] = "{\"schema_version\":1,\"effect\":\"static\",\"colors\":[0,6,12,18],\"brightness\":[0,1,2,3]}";
    assert(!parse_profile(seed, sizeof(seed) - 1, &profile));
    for (int iteration = 0; iteration < 20000; ++iteration) {
        size_t length = (size_t)iteration % sizeof(bytes);
        if (iteration % 3 == 0) {
            length = sizeof(seed) - 1;
            memcpy(bytes, seed, length);
            for (int mutation = 0; mutation < 3; ++mutation) {
                random_value = random_value * 1664525u + 1013904223u;
                bytes[random_value % length] = (unsigned char)(random_value >> 24);
            }
        } else if (iteration % 3 == 1) {
            length %= sizeof(seed);
            memcpy(bytes, seed, length);
        } else for (size_t i = 0; i < length; ++i) { random_value = random_value * 1664525u + 1013904223u; bytes[i] = (unsigned char)(random_value >> 24); }
        (void)parse_profile(bytes, length, &profile);
    }
    puts("PASS C backlight: 400 profiles, all 20 short/error/signal report-failure positions, 20000 bounded parser fuzz inputs");
    return 0;
}
