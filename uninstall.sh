#!/bin/bash
# SPDX-License-Identifier: MIT
# Remove matching release files only. Modules must already be unloaded explicitly.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
version=$(cat "$root/VERSION")
case "$version" in ''|*[!a-zA-Z0-9.+:~_-]*) exit 2 ;; esac
selection=cli
prefix=/usr/local
while (($#)); do
    case "$1" in
        --cli) selection=cli; shift ;;
        --drivers) selection=drivers; shift ;;
        --gui) selection=gui; shift ;;
        --all) selection=all; shift ;;
        --prefix) test "$#" -ge 2 || exit 2; prefix=$2; shift 2 ;;
        --version) cat "$root/VERSION"; exit 0 ;;
        -h|--help) echo 'Usage: ./uninstall.sh [--cli|--drivers|--gui|--all] [--prefix /usr/local]'; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
case "$prefix" in /*) ;; *) echo 'Prefix must be absolute.' >&2; exit 2 ;; esac
test "$prefix" != / || { echo 'Refusing a filesystem-root prefix.' >&2; exit 2; }
want_cli=0; want_drivers=0; want_gui=0
case "$selection" in
    all) want_cli=1; want_drivers=1; want_gui=1 ;;
    cli) want_cli=1 ;;
    drivers) want_drivers=1 ;;
    gui) want_gui=1 ;;
esac
if test "$want_cli" = 1; then sh "$root/userspace/uninstall.sh" "$prefix" --check; fi
if test "$want_gui" = 1; then bash "$root/scripts/install-gui.sh" --remove-check "$prefix"; fi
if test "$want_drivers" = 1; then
    test "$(id -u)" = 0 || { echo 'Run driver removal with sudo.' >&2; exit 1; }
    command -v dkms >/dev/null
    for module in y720_cooling y720_hid; do
        if test -d "/sys/module/$module"; then
            echo "Unload $module deliberately first; stop curves and select fan auto before unloading." >&2
            exit 1
        fi
    done
fi
if test "$want_cli" = 1; then sh "$root/userspace/uninstall.sh" "$prefix"; fi
if test "$want_drivers" = 1; then
    for module in y720-cooling y720-hid; do
        if test -e "/var/lib/dkms/$module/$version"; then dkms remove -m "$module" -v "$version" --all; fi
    done
fi
if test "$want_gui" = 1; then
    bash "$root/scripts/install-gui.sh" --remove "$prefix"
fi
echo 'Removed selected release files/registrations; retained source directories and device settings.'
