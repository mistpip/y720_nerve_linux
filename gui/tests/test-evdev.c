/* SPDX-License-Identifier: MIT */
/* Feed an anonymous pipe, never /dev/input, into the event dispatch code. */
#include "../src/y720-evdev.c"

static guint launches;
static void launch(gpointer unused) { (void)unused; launches++; }
static guint captured;
static void capture_key(guint code, gpointer unused)
{
    (void)unused;
    if (code == KEY_LEFTCTRL || code == KEY_LEFTSHIFT)
        return;
    captured = code;
    y720_evdev_capture_stop();
}

static void send_event(int fd, unsigned short type, unsigned short code, int value)
{
    struct input_event event = {.type=type, .code=code, .value=value};
    g_assert_cmpint(write(fd, &event, sizeof(event)), ==, sizeof(event));
}

int main(void)
{
    int pipefd[2];
    g_assert_cmpint(pipe2(pipefd, O_NONBLOCK | O_CLOEXEC), ==, 0);
    ev_fd = pipefd[0];
    keys[0] = (EvdevKey){KEY_PROG1, launch, NULL, FALSE};
    key_count = 1;
    /* NumLock-on digits and the media layer must never invoke the launcher. */
    const guint ordinary[] = {KEY_NUMLOCK, KEY_KP7, KEY_KP8, KEY_KP9, KEY_KPSLASH,
                              KEY_PLAYPAUSE, KEY_PREVIOUSSONG, KEY_NEXTSONG};
    for (guint i = 0; i < G_N_ELEMENTS(ordinary); i++) {
        send_event(pipefd[1], EV_KEY, ordinary[i], 1);
        send_event(pipefd[1], EV_KEY, ordinary[i], 0);
    }
    ev_event(NULL, G_IO_IN, NULL);
    g_assert_cmpuint(launches, ==, 0);
    for (guint i = 0; i < 3; i++) {
        send_event(pipefd[1], EV_KEY, KEY_PROG1, 1);
        send_event(pipefd[1], EV_KEY, KEY_PROG1, 2);
        send_event(pipefd[1], EV_KEY, KEY_PROG1, 0);
        ev_event(NULL, G_IO_IN, NULL);
    }
    g_assert_cmpuint(launches, ==, 3);
    /* Fn+Space is a dedicated input event, never an ordinary Space press. */
    keys[1] = (EvdevKey){KEY_KBDILLUMTOGGLE, launch, NULL, FALSE};
    key_count = 2;
    send_event(pipefd[1], EV_KEY, KEY_SPACE, 1);
    send_event(pipefd[1], EV_KEY, KEY_SPACE, 0);
    send_event(pipefd[1], EV_KEY, KEY_KBDILLUMTOGGLE, 1);
    send_event(pipefd[1], EV_KEY, KEY_KBDILLUMTOGGLE, 2);
    send_event(pipefd[1], EV_KEY, KEY_KBDILLUMTOGGLE, 0);
    ev_event(NULL, G_IO_IN, NULL);
    g_assert_cmpuint(launches, ==, 4);
    send_event(pipefd[1], EV_SYN, SYN_DROPPED, 0);
    send_event(pipefd[1], EV_KEY, KEY_PROG1, 1);
    send_event(pipefd[1], EV_SYN, SYN_REPORT, 0);
    ev_event(NULL, G_IO_IN, NULL);
    g_assert_cmpuint(launches, ==, 4);
    g_assert_false(keys[0].pressed);
    g_assert_cmpint(ev_event(NULL, G_IO_HUP, NULL), ==, G_SOURCE_REMOVE);
    g_assert_cmpint(ev_fd, ==, -1);
    close(pipefd[1]);
    g_assert_cmpint(pipe2(pipefd, O_NONBLOCK | O_CLOEXEC), ==, 0);
    cap_fd = pipefd[0];
    cap_state = (CaptureState){capture_key, NULL};
    ev_suspended = TRUE;
    send_event(pipefd[1], EV_KEY, KEY_LEFTCTRL, 1);
    send_event(pipefd[1], EV_KEY, KEY_LEFTSHIFT, 1);
    send_event(pipefd[1], EV_KEY, KEY_KBDILLUMTOGGLE, 1);
    g_assert_cmpint(capture_event(NULL, G_IO_IN, NULL), ==, G_SOURCE_REMOVE);
    g_assert_cmpuint(captured, ==, KEY_KBDILLUMTOGGLE);
    g_assert_cmpint(cap_fd, ==, -1);
    g_assert_false(ev_suspended);
    close(pipefd[1]);
    y720_evdev_capture_stop();
    y720_evdev_stop();
    return 0;
}
