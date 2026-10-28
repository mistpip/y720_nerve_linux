#!/bin/bash
# SPDX-License-Identifier: MIT
# Setup helper, input only. Never bind the independent I2C lighting interface.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
case "${1:-}" in
    --version) cat "$root/VERSION"; exit 0 ;;
    status|bind|restore) action=$1 ;;
    *) echo 'Usage: scripts/keyboard-bind.sh status|bind|restore|--version' >&2; exit 2 ;;
esac
test "$#" = 1 || exit 2
check_dmi() { test "$(<"/sys/class/dmi/id/$1")" = "$2" || { echo "Unsupported DMI: $1" >&2; exit 3; }; }
check_dmi sys_vendor LENOVO
check_dmi product_name 80Y4
check_dmi product_version Y720-15IKB
check_dmi bios_version 4GCN38WW
count=0
device=
for candidate in /sys/bus/hid/devices/0003:048D:C100.*; do
    test -d "$candidate" || continue
    resolved=$(readlink -f -- "$candidate")
    case "$resolved" in */virtual/*) continue ;; esac
    count=$((count+1))
    device=$candidate
done
test "$count" = 1 || { echo 'Expected exactly one physical Y720 USB keyboard.' >&2; exit 3; }
digest=$(sha256sum -- "$device/report_descriptor")
test "${digest%% *}" = 57ad1b4c6e7c9d18fbc021163c90ae031410f0649974c96170e447d53f6b61eb || {
    echo 'Unverified keyboard report descriptor.' >&2; exit 3;
}
driver_name() {
    if test -L "$device/driver"; then basename -- "$(readlink -f -- "$device/driver")"; else echo none; fi
}
driver=$(driver_name)
case "$driver" in none|hid-generic|y720-hid) ;; *) echo 'Unexpected keyboard driver; refusing to rebind.' >&2; exit 3 ;; esac
if test "$action" != status; then
    test "$(id -u)" = 0 || { echo 'Rebinding requires root.' >&2; exit 4; }
    if test "$action" = restore; then
        if test -d /sys/module/y720_hid; then modprobe -r y720_hid; fi
        wanted=hid-generic
    else
        wanted=y720-hid
        test -d /sys/bus/hid/drivers/y720-hid || { echo 'Load y720_hid explicitly first.' >&2; exit 3; }
    fi
    driver=$(driver_name)
    if test "$driver" != "$wanted"; then
        if test "$driver" != none; then printf '%s' "${device##*/}" > "$device/driver/unbind"; fi
        if ! printf '%s' "${device##*/}" > "/sys/bus/hid/drivers/$wanted/bind"; then
            echo 'Input binding failed; attempting generic input restoration.' >&2
            if test -d /sys/module/y720_hid; then modprobe -r y720_hid || true; fi
            if test "$(driver_name)" != hid-generic; then
                printf '%s' "${device##*/}" > /sys/bus/hid/drivers/hid-generic/bind || {
                    echo 'Generic keyboard restoration failed; reconnect/reboot may be required.' >&2;
                }
            fi
            exit 6
        fi
    fi
fi
printf 'device=%s\ndriver=%s\n' "${device##*/}" "$(driver_name)"
