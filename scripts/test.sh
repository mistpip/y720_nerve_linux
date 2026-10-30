#!/bin/bash
# SPDX-License-Identifier: MIT
# Pure/mocked tests only; no /dev, EC or real fan/backlight writes.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d /tmp/y720-tests-XXXXXX)
clang -Wall -Wextra -Werror -fsanitize=address,undefined "$root/fan/tests/test-fan-policy.c" -o "$test_dir/fan"
clang -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined "$root/fan/tests/test-thermal.c" -o "$test_dir/thermal"
clang -Wall -Wextra -Werror -fsanitize=address,undefined "$root/keyboard/tests/test_mapping.c" -o "$test_dir/keyboard"
"$test_dir/fan"
"$test_dir/thermal"
"$test_dir/keyboard"
make -C "$root/userspace" CC=clang test
python3 -m unittest discover -s "$root/tests" -v
bash -n "$root/install.sh" "$root/uninstall.sh" "$root/scripts/install-dkms.sh" "$root/scripts/keyboard-bind.sh"
sh -n "$root/userspace/install.sh" "$root/userspace/uninstall.sh"
bash -n "$root/scripts/install-gui.sh" "$root/scripts/build-debs.sh"
sh -n "$root/scripts/refresh-device-access.sh"
if command -v meson >/dev/null && pkg-config --exists gtk4 libadwaita-1 json-glib-1.0; then
    meson setup "$test_dir/gui" "$root/gui" --prefix=/usr \
        -Db_sanitize=address,undefined -Db_lundef=false
    ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 meson test -C "$test_dir/gui" --print-errorlogs
    meson compile -C "$test_dir/gui" y720-control
    Y720_TEST_GUI_BUILD="$test_dir/gui" python3 "$root/tests/test_gui_install.py" -v
else
    echo 'SKIP GTK tests: install GUI build dependencies to run the full suite.'
fi
echo "PASS local product tests. Temporary executables: $test_dir"
