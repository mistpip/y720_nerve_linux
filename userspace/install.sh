#!/bin/sh
# SPDX-License-Identifier: MIT
# Deliberate file installation only; no modules, sudo, services or hardware I/O.
set -eu
prefix=${1:-/usr/local}
case "$prefix" in /*) ;; *) echo 'Prefix must be absolute.' >&2; exit 2 ;; esac
test "$prefix" != / || { echo 'Refusing a filesystem-root prefix.' >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
case "${2:-}" in ''|--check) ;; *) exit 2 ;; esac
# Build as an ordinary user first. Do not invoke a compiler implicitly as root.
for name in y720-fan y720-backlight; do
    test -x "$root/userspace/build/$name" || {
        echo 'Build first: make -C userspace' >&2
        exit 1
    }
done
# Inspect every destination before changing anything. Never overwrite another program.
check() {
    if test -L "$2" || { test -e "$2" && ! cmp -s "$1" "$2"; }; then
        echo "Refusing to replace existing or symlink destination: $2" >&2
        exit 1
    fi
}
check "$root/userspace/build/y720-fan" "$prefix/bin/y720-fan"
check "$root/userspace/build/y720-backlight" "$prefix/bin/y720-backlight"
for name in CLI.md DEVELOPMENT.md; do
    check "$root/docs/$name" "$prefix/share/doc/y720-cli/$name"
done
test "${2:-}" != --check || exit 0
install -d "$prefix/bin" "$prefix/share/doc/y720-cli"
install -m755 "$root/userspace/build/y720-fan" "$prefix/bin/y720-fan"
install -m755 "$root/userspace/build/y720-backlight" "$prefix/bin/y720-backlight"
for name in CLI.md DEVELOPMENT.md; do
    install -m644 "$root/docs/$name" "$prefix/share/doc/y720-cli/$name"
done
echo "Installed y720-fan and y720-backlight under $prefix; no startup or hardware changes."
