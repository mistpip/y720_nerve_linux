#!/bin/sh
# SPDX-License-Identifier: MIT
# Re-evaluate uaccess only for the two supported Y720 devices. No rebind or input replay.
set -eu
if ! command -v udevadm >/dev/null 2>&1; then
    echo 'udevadm unavailable; device permissions require udev and an active logind seat.' >&2
    exit 0
fi
udevadm control --reload-rules || {
    echo 'Unable to reload udev rules; reconnect the keyboard or reboot after resolving udev.' >&2
    exit 0
}
for node in /sys/class/input/event*; do
    test -r "$node/device/name" || continue
    test "$(cat "$node/device/name")" = 'ITE Tech. Inc. ITE Device(8910) Keyboard' || continue
    test "$(cat "$node/device/id/bustype")" = 0003 || continue
    test "$(cat "$node/device/id/vendor")" = 048d || continue
    test "$(cat "$node/device/id/product")" = c100 || continue
    test "$(cat "$node/device/id/version")" = 0110 || continue
    udevadm trigger --action=change --settle -- "$node" || echo "Unable to refresh input permissions: $node" >&2
done
for node in /sys/class/hidraw/hidraw*; do
    test -e "$node/device" || continue
    device=$(readlink -f "$node/device") || continue
    case "${device##*/}" in
        0018:048D:837A.*)
            udevadm trigger --action=change --settle -- "$node" || echo "Unable to refresh HID permissions: $node" >&2 ;;
    esac
done
# No blanket udev trigger, chmod 666, input-group membership or module loading.
