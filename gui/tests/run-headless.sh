#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
if ! command -v gtk4-broadwayd >/dev/null; then
    echo 'SKIP: gtk4-broadwayd required for isolated GTK lifecycle tests'
    exit 77
fi
task_runtime=$(mktemp -d /tmp/y720-gui-test-XXXXXX)
export XDG_RUNTIME_DIR="$task_runtime"
export XDG_CONFIG_HOME="$task_runtime/config"
export GDK_BACKEND=broadway BROADWAY_DISPLAY=:1 GSK_RENDERER=cairo
export GTK_A11Y=none GIO_USE_VFS=local
# Fontconfig/GTK retain process-global caches; this suite checks invalid access,
# undefined behaviour and lifecycle criticals, not global-cache leak accounting.
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"
gtk4-broadwayd --unixsocket="$task_runtime/http.socket" :1 >"$task_runtime/broadway.log" 2>&1 &
server=$!
trap 'kill "$server" 2>/dev/null || true' EXIT HUP INT TERM
attempt=0
while ! test -S "$task_runtime/broadway2.socket"; do
    attempt=$((attempt + 1))
    test "$attempt" -le 30 || { cat "$task_runtime/broadway.log"; exit 1; }
    sleep 0.1
done
dbus-run-session -- "$@"
