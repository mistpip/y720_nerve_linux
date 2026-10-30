# SPDX-License-Identifier: MIT
"""Verify release permissions without relying on the developer's user or device ACLs."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GuiPermissions(unittest.TestCase):
    def test_generated_admin_allowlist_is_exact_and_prefix_specific(self):
        for prefix in ("/usr/bin", "/usr/local/bin"):
            with tempfile.TemporaryDirectory(prefix="y720-auth-test-") as directory:
                path = Path(directory) / "sudoers"
                subprocess.run(["python3", str(ROOT / "gui/tools/make-sudoers.py"), prefix, str(path)],
                               check=True)
                if shutil.which("visudo"):
                    subprocess.run(["visudo", "-cf", str(path)], check=True, capture_output=True)
                expected = {
                    f"{prefix}/y720-fan --json status", f"{prefix}/y720-fan --json mode auto",
                    f"{prefix}/y720-fan --json mode boost", f"{prefix}/y720-backlight --json status",
                    f"{prefix}/y720-backlight --json apply -",
                    f"{prefix}/y720-fan --json curve apply -", f"{prefix}/y720-fan --json curve stop",
                }
                expected.update(f"{prefix}/y720-fan --json set --rpm {rpm}" for rpm in range(1500, 5101, 100))
                actual = set()
                for line in path.read_text().splitlines():
                    if line.startswith("#"):
                        continue
                    user_list, command = line.split(" ALL=(root) NOPASSWD: ", 1)
                    self.assertEqual(user_list, "%sudo, %wheel")
                    self.assertNotIn("*", command)
                    actual.add(command)
                self.assertEqual(actual, expected)
                self.assertNotIn(f"{prefix}/y720-backlight --json apply /etc/shadow", actual)

    def test_device_access_is_active_seat_and_exact_keyboard_only(self):
        path = ROOT / "gui/data/70-y720-control.rules"
        text = path.read_text()
        if shutil.which("udevadm"):
            subprocess.run(["udevadm", "verify", str(path)], check=True, capture_output=True)
        rules = [line for line in text.splitlines() if line and not line.startswith("#")]
        self.assertEqual(len(rules), 2)
        keyboard = next(rule for rule in rules if 'SUBSYSTEM=="input"' in rule)
        for field in ('KERNEL=="event[0-9]*"', 'ATTRS{id/bustype}=="0003"',
                      'ATTRS{id/vendor}=="048d"', 'ATTRS{id/product}=="c100"',
                      'ATTRS{id/version}=="0110"', 'TAG+="uaccess"'):
            self.assertIn(field, keyboard)
        self.assertIn('ATTRS{name}=="ITE Tech. Inc. ITE Device(8910) Keyboard"', keyboard)
        self.assertNotIn('MODE="0666"', text)
        self.assertNotIn('GROUP="input"', text)
        self.assertLess(int(path.name.split("-")[0]), 73)  # Before logind's seat-late rule.

    def test_installers_refresh_only_matched_devices(self):
        script = (ROOT / "scripts/refresh-device-access.sh").read_text()
        for field in ("id/bustype", "id/vendor", "id/product", "id/version"):
            self.assertIn(field, script)
        triggers = [line for line in script.splitlines() if "udevadm trigger " in line]
        self.assertEqual(len(triggers), 2)
        self.assertTrue(all('trigger --action=change --settle -- "$node"' in line for line in triggers))
        self.assertIn('0018:048D:837A.*', script)
        self.assertIn('if test -z "$destdir"', (ROOT / "scripts/install-gui.sh").read_text())
        self.assertIn('scripts/refresh-device-access.sh', (ROOT / "scripts/build-debs.sh").read_text())


if __name__ == "__main__":
    unittest.main()
