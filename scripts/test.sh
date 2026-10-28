#!/bin/bash
# SPDX-License-Identifier: MIT
# Pure/mocked tests only; no /dev, EC or real fan/backlight writes.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d /tmp/y720-tests-XXXXXX)
clang -Wall -Wextra -Werror -fsanitize=address,undefined "$root/fan/tests/test-fan-policy.c" -o "$test_dir/fan"
clang -Wall -Wextra -Werror -fsanitize=address,undefined "$root/keyboard/tests/test_mapping.c" -o "$test_dir/keyboard"
"$test_dir/fan"
"$test_dir/keyboard"
make -C "$root/userspace" CC=clang test
python3 -m unittest discover -s "$root/tests" -v
bash -n "$root/install.sh" "$root/uninstall.sh" "$root/scripts/install-dkms.sh" "$root/scripts/keyboard-bind.sh"
sh -n "$root/userspace/install.sh" "$root/userspace/uninstall.sh"
echo "PASS local product tests. Temporary executables: $test_dir"
