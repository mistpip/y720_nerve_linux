#!/bin/bash
# SPDX-License-Identifier: MIT
set -euo pipefail
action=${1:-}
kernel_release=${2:-}
case "$action" in --check|--install) ;; *) echo 'Internal usage: --check|--install KERNEL' >&2; exit 2 ;; esac
case "$kernel_release" in ''|*[!a-zA-Z0-9._+-]*) echo 'Invalid kernel release.' >&2; exit 2 ;; esac
test "$(id -u)" = 0 || { echo 'Run DKMS installation with sudo.' >&2; exit 1; }
command -v dkms >/dev/null || { echo 'DKMS is required.' >&2; exit 1; }
test -d "/lib/modules/$kernel_release/build" || { echo 'Matching kernel headers are required; do not upgrade the kernel implicitly.' >&2; exit 1; }
if grep -q '^CONFIG_CC_IS_CLANG=y' "/lib/modules/$kernel_release/build/.config"; then
    for compiler_tool in clang ld.lld; do
        command -v "$compiler_tool" >/dev/null || { echo "Missing matching toolchain: $compiler_tool" >&2; exit 1; }
    done
else
    command -v gcc >/dev/null || { echo 'Missing matching GCC toolchain.' >&2; exit 1; }
fi
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
for module in y720-cooling y720-hid; do
    for destination in "/usr/src/$module-0.1" "/var/lib/dkms/$module/0.1"; do
        if test -e "$destination" || test -L "$destination"; then
            echo "Refusing to overwrite $destination. Inspect/remove the previous registration explicitly." >&2
            exit 1
        fi
    done
done
test "$action" = --install || exit 0
for component in fan keyboard; do
    if test "$component" = fan; then
        module=y720-cooling
        sources=(y720_cooling.c y720-native.h y720-fan-policy.h)
    else
        module=y720-hid
        sources=(y720_hid.c y720-mapping.h)
    fi
    destination="/usr/src/$module-0.1"
    install -d "$destination/kernel"
    install -m644 "$root/$component/dkms.conf" "$destination/dkms.conf"
    install -m644 "$root/$component/kernel/Makefile" "$destination/kernel/Makefile"
    install -m644 "$root/LICENSES/GPL-2.0.txt" "$destination/kernel/COPYING"
    for name in "${sources[@]}"; do install -m644 "$root/$component/kernel/$name" "$destination/kernel/$name"; done
    dkms add -m "$module" -v 0.1
    dkms build -m "$module" -v 0.1 -k "$kernel_release"
    dkms install -m "$module" -v 0.1 -k "$kernel_release"
done
# No automatic cleanup on failure: retain partial DKMS state for explicit inspection.
# No modprobe, initramfs rebuild, vendor replacement, or modules-load configuration.
