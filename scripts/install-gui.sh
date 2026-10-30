#!/bin/bash
# SPDX-License-Identifier: MIT
# Manifest-based GUI installation. DESTDIR confines all writes for packaging/tests.
set -euo pipefail
action=${1:-}
prefix=${2:-/usr/local}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${Y720_GUI_BUILD:-"$root/gui/build"}
destdir=${DESTDIR:-}
case "$action" in --check|--install|--remove-check|--remove) ;; *) exit 2 ;; esac
case "$prefix" in /*) ;; *) echo 'Prefix must be absolute' >&2; exit 2 ;; esac
case "$prefix" in /|*\\*|*$'\n'*|*$'\r'*) echo 'Unsafe prefix' >&2; exit 2 ;; esac
prefix=$(realpath -m -- "$prefix")
test "$prefix" != / || exit 2
if test -n "$destdir"; then
    case "$destdir" in /*) ;; *) exit 2 ;; esac
    destdir=$(realpath -m -- "$destdir")
    test "$destdir" != / || exit 2
else
    test "$(id -u)" = 0 || { echo 'GUI system files require root; use DESTDIR for staging.' >&2; exit 1; }
    # A privileged executable must never be installed below a user-writable parent.
    parent=$prefix
    while test "$parent" != /; do
        if test -e "$parent"; then
            owner=$(stat -c %u -- "$parent")
            mode=$(stat -c %a -- "$parent")
            test "$owner" = 0 && (( (8#$mode & 0022) == 0 )) || {
                echo "Untrusted installation parent: $parent" >&2; exit 1;
            }
        fi
        parent=$(dirname -- "$parent")
    done
fi
manifest="$destdir$prefix/share/y720-control/install-manifest.sha256"
allowed() {
    case "$1" in
        "$prefix/bin/y720-control"|"$prefix/share/applications/org.y720.Control.desktop"|\
        "$prefix/share/icons/hicolor/128x128/apps/org.y720.Control.png"|\
        "$prefix/share/icons/hicolor/256x256/apps/org.y720.Control.png"|\
        "$prefix/share/metainfo/org.y720.Control.metainfo.xml"|\
        "$prefix/share/doc/y720-control/copyright"|\
        "$prefix/share/doc/y720-control/LICENSES/MIT.txt"|\
        "$prefix/share/doc/y720-control/LICENSES/GPL-2.0.txt"|\
        /etc/sudoers.d/y720-control|/usr/lib/udev/rules.d/70-y720-control.rules) return 0 ;;
        *) return 1 ;;
    esac
}
safe_path() {
    local parent="$destdir$1"
    while test "$parent" != /; do
        test ! -L "$parent" || { echo "Refusing symlink: $parent" >&2; exit 1; }
        parent=$(dirname -- "$parent")
    done
}
check_manifest() {
    test ! -L "$manifest" || exit 1
    test -f "$manifest" || return 0
    while IFS= read -r line; do
        local hash=${line%% *} logical=${line#*  }
        [[ "$hash" =~ ^[a-f0-9]{64}$ ]] && allowed "$logical" || exit 1
        safe_path "$logical"
        if test -e "$destdir$logical"; then
            local actual
            actual=$(sha256sum -- "$destdir$logical")
            test "${actual%% *}" = "$hash" || {
                echo "Refusing modified/foreign GUI file: $logical" >&2; exit 1;
            }
        fi
    done < "$manifest"
}
check_manifest
legacy_rules=/usr/share/polkit-1/rules.d/49-y720-control.rules
legacy_policy=/usr/share/polkit-1/actions/org.y720.Control.policy
legacy_files=()
for logical in "$legacy_rules" "$legacy_policy"; do
    safe_path "$logical"
    if test -e "$destdir$logical"; then
        actual=$(sha256sum -- "$destdir$logical")
        case "$logical:${actual%% *}" in
            "$legacy_rules":ffc9b7b02bda77981d3923fcc0d3a8b8eabb140167ab65f703c9aca5f2c675bd|\
            "$legacy_policy":217e53eca9276d79dea37e8ec5d46c287500210f8df31912d50052cbdc36701f|\
            "$legacy_policy":ea5e530a9f4930b7adf89bdd7e8e6519255372c04bd5234520770da63e59202d)
                legacy_files+=("$logical") ;;
            *) echo "Modified legacy policy requires explicit review: $logical" >&2; exit 1 ;;
        esac
    fi
done
if [[ "$action" = --remove* ]]; then
    if test ! -f "$manifest"; then
        echo 'No GUI installation manifest; no files removed. Inspect legacy installations separately.' >&2
        exit 1
    fi
    test "$action" != --remove-check || exit 0
    while IFS= read -r line; do
        logical=${line#*  }
        test ! -e "$destdir$logical" || rm -- "$destdir$logical"
    done < "$manifest"
    rm -- "$manifest"
    exit 0
fi
test -x "$build/y720-control" || { echo 'Build GUI as an ordinary user first: make gui' >&2; exit 1; }
built_prefix=$(python3 -c 'import json,sys; print(next(x["value"] for x in json.load(open(sys.argv[1])) if x["name"]=="prefix"))' "$build/meson-info/intro-buildoptions.json")
test "$built_prefix" = "$prefix" || {
    echo "GUI was built for $built_prefix, requested $prefix. Rebuild with: make gui PREFIX='$prefix'" >&2; exit 1;
}
stage=$(mktemp -d /tmp/y720-gui-stage-XXXXXX)
meson install -C "$build" --destdir "$stage" --no-rebuild --only-changed >/dev/null
command -v visudo >/dev/null || { echo 'visudo required to validate GUI authorization' >&2; exit 1; }
visudo -cf "$stage/etc/sudoers.d/y720-control" >/dev/null
mapfile -d '' files < <(find "$stage" -type f -print0)
for file in "${files[@]}"; do
    logical=${file#"$stage"}
    allowed "$logical" || { echo "Unexpected GUI payload: $logical" >&2; exit 1; }
    safe_path "$logical"
    if test -e "$destdir$logical" && ! cmp -s "$file" "$destdir$logical"; then
        actual=$(sha256sum -- "$destdir$logical")
        if ! test -f "$manifest" || ! grep -Fxq -- "${actual%% *}  $logical" "$manifest"; then
            echo "Unowned GUI destination: $logical" >&2; exit 1
        fi
    fi
done
test "$action" != --check || exit 0
safe_path "$prefix/share/y720-control/install-manifest.sha256"
install -d -m755 -- "$(dirname -- "$manifest")"
new_manifest=$(mktemp "${manifest}.XXXXXX")
for logical in "${legacy_files[@]}"; do rm -- "$destdir$logical"; done
for file in "${files[@]}"; do
    logical=${file#"$stage"}
    mode=644
    case "$logical" in "$prefix/bin/y720-control") mode=755 ;; /etc/sudoers.d/y720-control) mode=440 ;; esac
    install -D -m"$mode" -- "$file" "$destdir$logical"
    hash=$(sha256sum -- "$destdir$logical")
    printf '%s  %s\n' "${hash%% *}" "$logical" >> "$new_manifest"
done
chmod 644 "$new_manifest"
mv -f -- "$new_manifest" "$manifest"
if test -z "$destdir"; then
    sh "$root/scripts/refresh-device-access.sh"
fi
echo 'GUI installed; no modules, hardware or startup configuration changed.'
