# SPDX-License-Identifier: MIT
"""Isolated manifest installer tests; never write the host's /etc or /usr."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BUILD = os.environ.get("Y720_TEST_GUI_BUILD")


@unittest.skipUnless(BUILD, "set Y720_TEST_GUI_BUILD to a GUI build with prefix=/usr")
class GuiInstall(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="y720-install-test-")
        self.addCleanup(self.temp.cleanup)
        self.dest = Path(self.temp.name)
        self.env = dict(os.environ, DESTDIR=str(self.dest), Y720_GUI_BUILD=BUILD)

    def run_installer(self, action, prefix="/usr", success=True):
        result = subprocess.run(
            ["bash", str(ROOT / "scripts/install-gui.sh"), action, prefix],
            env=self.env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            timeout=40,
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_install_repeat_remove_preserves_configuration(self):
        config = self.dest / "home/example/.config/y720-control/settings.json"
        config.parent.mkdir(parents=True)
        config.write_text('{"unchanged": true}\n')
        self.run_installer("--check")
        self.assertFalse((self.dest / "usr/bin/y720-control").exists())
        self.run_installer("--install")
        self.run_installer("--install")
        sudoers = self.dest / "etc/sudoers.d/y720-control"
        self.assertEqual(sudoers.stat().st_mode & 0o777, 0o440)
        self.assertTrue((self.dest / "usr/share/doc/y720-control/LICENSES/MIT.txt").is_file())
        self.run_installer("--remove-check")
        self.run_installer("--remove")
        self.assertEqual([p for p in self.dest.rglob("*") if p.is_file()], [config])

    def test_modified_owned_file_aborts_before_removal(self):
        self.run_installer("--install")
        binary = self.dest / "usr/bin/y720-control"
        original = binary.read_bytes()
        binary.write_bytes(original + b"modified")
        self.run_installer("--remove", success=False)
        self.assertTrue((self.dest / "etc/sudoers.d/y720-control").is_file())
        binary.write_bytes(original)
        self.run_installer("--remove")

    def test_foreign_destination_is_not_overwritten(self):
        binary = self.dest / "usr/bin/y720-control"
        binary.parent.mkdir(parents=True)
        binary.write_text("foreign\n")
        self.run_installer("--install", success=False)
        self.assertEqual(binary.read_text(), "foreign\n")
        self.assertFalse((self.dest / "etc/sudoers.d/y720-control").exists())

    def test_symlink_parent_is_rejected(self):
        outside = self.dest / "outside"
        outside.mkdir()
        (self.dest / "usr").symlink_to(outside, target_is_directory=True)
        self.run_installer("--install", success=False)
        self.assertEqual(list(outside.iterdir()), [])

    def test_build_prefix_must_match(self):
        self.run_installer("--install", "/opt/y720-test", success=False)
        self.assertEqual(list(self.dest.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
