#!/bin/bash
# SPDX-License-Identifier: MIT
# Install only. Never load modules, change lighting/fan modes, or enable services.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
selection=all
prefix=/usr/local
kernel_release=$(uname -r)
while (($#)); do
    case "$1" in
        --cli) selection=cli; shift ;;
        --drivers) selection=drivers; shift ;;
        --gui) selection=gui; shift ;;
        --all) selection=all; shift ;;
        --prefix) test "$#" -ge 2 || exit 2; prefix=$2; shift 2 ;;
        --kernel) test "$#" -ge 2 || exit 2; kernel_release=$2; shift 2 ;;
        --version) cat "$root/VERSION"; exit 0 ;;
        -h|--help)
            echo 'Usage: ./install.sh [--all|--cli|--drivers|--gui] [--prefix /usr/local] [--kernel RELEASE]'
            echo 'Default: CLIs, two DKMS drivers and the GUI; no module loading or startup.'
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
case "$prefix" in /*) ;; *) echo 'Prefix must be absolute.' >&2; exit 2 ;; esac
test "$prefix" != / || { echo 'Refusing a filesystem-root prefix.' >&2; exit 2; }
case "$kernel_release" in ''|*[!a-zA-Z0-9._+-]*) echo 'Invalid kernel release.' >&2; exit 2 ;; esac
# Check all requested components before installation begins.
want_cli=0; want_drivers=0; want_gui=0
case "$selection" in
    all) want_cli=1; want_drivers=1; want_gui=1 ;;
    cli) want_cli=1 ;;
    drivers) want_drivers=1 ;;
    gui) want_gui=1 ;;
esac
if test "$want_cli" = 1; then
    sh "$root/userspace/install.sh" "$prefix" --check
fi
if test "$want_drivers" = 1; then
    bash "$root/scripts/install-dkms.sh" --check "$kernel_release"
fi
if test "$want_gui" = 1; then
    bash "$root/scripts/install-gui.sh" --check "$prefix"
fi
if test "$want_cli" = 1; then sh "$root/userspace/install.sh" "$prefix"; fi
if test "$want_drivers" = 1; then bash "$root/scripts/install-dkms.sh" --install "$kernel_release"; fi
if test "$want_gui" = 1; then bash "$root/scripts/install-gui.sh" --install "$prefix"; fi
echo 'Installation complete. No modules loaded; no hardware or boot configuration changed.'
