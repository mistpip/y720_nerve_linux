/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *action = argc > 1 ? argv[1] : "ok";
    if (!strcmp(action, "--json")) {
        if (getenv("Y720_FIXTURE_HANG")) {
            signal(SIGTERM, SIG_IGN);
            const char *path = getenv("Y720_FIXTURE_PIDS");
            FILE *file = path ? fopen(path, "a") : NULL;
            if (file) { fprintf(file, "%ld\n", (long)getpid()); fclose(file); }
            for (;;) pause();
        }
        int light = strstr(argv[0], "backlight") != NULL;
        const char *command = argc > 2 ? argv[2] : "status";
        const char *program = light ? "y720-backlight" : "y720-fan";
        if (!light && !strcmp(command, "status") && getenv("Y720_FIXTURE_QUERY_FAIL")) {
            fprintf(stderr, "{\"schema_version\":1,\"program\":\"y720-fan\",\"version\":\"%s\",\"ok\":false,"
                    "\"error\":{\"code\":\"io_error\",\"message\":\"injected disconnect\"}}\n", Y720_VERSION);
            return 6;
        }
        if (!light && !strcmp(command, "curve") && getenv("Y720_FIXTURE_CURVE_DELAY"))
            usleep(150000);
        if (!light && !strcmp(command, "curve") && getenv("Y720_FIXTURE_CURVE_FAIL")) {
            fprintf(stderr, "{\"schema_version\":1,\"program\":\"y720-fan\",\"version\":\"%s\",\"ok\":false,"
                    "\"error\":{\"code\":\"write_uncertain\",\"message\":\"injected failure\"}}\n", Y720_VERSION);
            return 7;
        }
        printf("{\"schema_version\":1,\"program\":\"%s\",\"version\":\"%s\",\"ok\":true,\"data\":",
               program, Y720_VERSION);
        if (strcmp(command, "status") && getenv("Y720_FIXTURE_BAD_ACK")) {
            puts("{}}");
            return 0;
        }
        if (light) {
            puts(!strcmp(command, "status") ?
                "{\"device\":\"mock\",\"hid_device\":\"mock\",\"driver\":\"hid-generic\","
                "\"descriptor_sha256\":\"mock\",\"report_bytes\":7,\"state_known\":false,\"profile\":null}}" :
                "{\"reports_completed\":5,\"transport_accepted\":true,\"visual_verified\":false,\"state_known\":false}}");
        } else {
            int write = strcmp(command, "status") != 0;
            int raw = 2, points[6] = {1500,1800,2400,3200,4200,5100};
            const char *state = getenv("Y720_FIXTURE_CURVE_STATE");
            FILE *file = state ? fopen(state, "r") : NULL;
            if (file) {
                if (fscanf(file, "%d %d %d %d %d %d %d", &raw, &points[0], &points[1], &points[2], &points[3], &points[4], &points[5]) != 7) raw = 2;
                fclose(file);
            }
            if (!strcmp(command, "set")) raw = 1;
            else if (!strcmp(command, "mode")) raw = argc > 3 && !strcmp(argv[3], "boost") ? 0 : 2;
            else if (!strcmp(command, "curve") && argc > 3) {
                if (!strcmp(argv[3], "apply")) {
                    if (scanf("%d %d %d %d %d %d", &points[0], &points[1], &points[2], &points[3], &points[4], &points[5]) != 6) return 2;
                    raw = 3;
                } else raw = 1;
            }
            if (write && state) {
                file = fopen(state, "w");
                if (!file) return 1;
                fprintf(file, "%d %d %d %d %d %d %d\n", raw, points[0], points[1], points[2], points[3], points[4], points[5]);
                fclose(file);
            }
            const char *mode = raw == 0 ? "boost" : raw == 1 || raw == 3 ? "curve" : "auto";
            if (write) printf("{\"status\":");
            printf("{\"controls_available\":true,\"device\":\"mock\",\"driver_version\":\"%s\","
                   "\"mode\":\"%s\",\"mode_raw\":%d,\"target_rpm\":1500,\"pwm\":0,"
                   "\"fan_rpm\":[1500,1500],\"diagnostics\":{},\"temperature\":{"
                   "\"millidegrees\":%d,\"source\":\"x86_pkg_temp\"},"
                   "\"kernel_curve\":{\"supported\":true,\"active\":%s,\"engine\":\"linux-thermal\",\"error\":%d,"
                   "\"points\":[%d,%d,%d,%d,%d,%d],\"temperature_millidegrees\":null}}",
                   Y720_VERSION, mode, raw,
                   getenv("Y720_FIXTURE_CPU_MC") ? atoi(getenv("Y720_FIXTURE_CPU_MC")) : 55000,
                   raw == 3 ? "true" : "false",
                   getenv("Y720_FIXTURE_CURVE_ERROR") ? -61 : 0,
                   points[0], points[1], points[2], points[3], points[4], points[5]);
            puts(write ? ",\"settled\":false}}" : "}");
        }
        return 0;
    }
    if (!strcmp(action, "hang")) {
        signal(SIGTERM, SIG_IGN);
        for (;;) pause();
    }
    if (!strcmp(action, "invalid")) { puts("not JSON"); return 0; }
    if (!strcmp(action, "permission")) {
        fprintf(stderr, "{\"schema_version\":1,\"program\":\"y720-fan\",\"version\":\"%s\","
                "\"ok\":false,\"error\":{\"code\":\"permission\",\"message\":\"denied\"}}\n", Y720_VERSION);
        return 4;
    }
    printf("{\"schema_version\":%d,\"program\":\"%s\",\"version\":\"%s\",\"ok\":true,\"data\":{}}\n",
           !strcmp(action, "schema") ? 2 : 1,
           !strcmp(action, "program") ? "wrong-program" : "y720-fan",
           !strcmp(action, "version") ? "999" : Y720_VERSION);
    return !strcmp(action, "contradiction") ? 3 : 0;
}
