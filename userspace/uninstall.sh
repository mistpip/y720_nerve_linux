#!/bin/sh
# SPDX-License-Identifier: MIT
# Remove only unchanged files belonging to this source release; never recurse.
set -eu
prefix=${1:-/usr/local}
case "$prefix" in /*) ;; *) echo 'Prefix must be absolute.' >&2; exit 2 ;; esac
test "$prefix" != / || { echo 'Refusing a filesystem-root prefix.' >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
case "${2:-}" in ''|--check) ;; *) exit 2 ;; esac
check() {
    if test -L "$2" || { test -e "$2" && ! cmp -s "$1" "$2"; }; then
        echo "Refusing to delete modified or foreign file: $2" >&2
        exit 1
    fi
}
check "$root/userspace/build/y720-fan" "$prefix/bin/y720-fan"
check "$root/userspace/build/y720-backlight" "$prefix/bin/y720-backlight"
for name in CLI.md DEVELOPMENT.md; do
    check "$root/docs/$name" "$prefix/share/doc/y720-cli/$name"
done
test "${2:-}" != --check || exit 0
for name in y720-fan y720-backlight; do
    if test -f "$prefix/bin/$name"; then rm -- "$prefix/bin/$name"; fi
done
for name in CLI.md DEVELOPMENT.md; do
    if test -f "$prefix/share/doc/y720-cli/$name"; then rm -- "$prefix/share/doc/y720-cli/$name"; fi
done
echo 'Removed only matching CLI/manual files; retained directories, drivers and device settings.'
