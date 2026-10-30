# SPDX-License-Identifier: MIT
"""Inspect built packages and exercise DKMS failure handling with a fake command."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DIST = os.environ.get("Y720_TEST_DEB_DIR")
VERSION = (ROOT / "VERSION").read_text().strip()


@unittest.skipUnless(DIST, "set Y720_TEST_DEB_DIR to inspect freshly built packages")
class DebPackages(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="y720-deb-test-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def extract(self, package, architecture):
        archive = Path(DIST) / f"{package}_{VERSION}_{architecture}.deb"
        destination = self.directory / package
        subprocess.run(["dpkg-deb", "-R", str(archive), str(destination)], check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return archive, destination

    def test_payload_permissions_versions_and_licenses(self):
        architecture = subprocess.check_output(["dpkg", "--print-architecture"], text=True).strip()
        for package, arch in (("y720-control", architecture), ("y720-cooling-dkms", "all"),
                              ("y720-hid-dkms", "all")):
            archive, destination = self.extract(package, arch)
            self.assertEqual(subprocess.check_output(["dpkg-deb", "-f", str(archive), "Version"],
                                                    text=True).strip(), VERSION)
            for path in destination.rglob("*"):
                self.assertFalse(path.is_symlink(), str(path))
                self.assertEqual(path.stat().st_mode & 0o022, 0, str(path))
                self.assertNotIn(path.suffix, {".exe", ".rar", ".rom", ".key", ".pem", ".ko"})
                self.assertNotIn("mock-cli", path.name)
            doc = destination / "usr/share/doc" / package
            self.assertTrue((doc / "copyright").is_file())
            for license_name in ("MIT.txt", "GPL-2.0.txt"):
                self.assertTrue((doc / "LICENSES" / license_name).is_file())
            for name in ("postinst", "prerm", "postrm"):
                script = destination / "DEBIAN" / name
                if script.exists():
                    subprocess.run(["sh", "-n", str(script)], check=True)
            if package == "y720-control":
                self.assertEqual({p.name for p in (doc / 'docs').glob('*.md')}, {'CLI.md','DEVELOPMENT.md'})
                self.assertTrue((doc / 'README.md').is_file())
                for name in ("y720-fan", "y720-backlight", "y720-control"):
                    self.assertTrue((destination / "usr/bin" / name).read_bytes().startswith(b"\x7fELF"))
                self.assertFalse((destination / 'usr/libexec').exists())
                self.assertFalse((destination / 'usr/lib/systemd').exists())
                self.assertNotIn('systemctl start', (destination / 'DEBIAN/postinst').read_text())
                sudoers = destination / "etc/sudoers.d/y720-control"
                self.assertEqual(sudoers.stat().st_mode & 0o777, 0o440)
                subprocess.run(["visudo", "-cf", str(sudoers)], check=True, stdout=subprocess.PIPE)
                self.assertFalse((destination / "usr/share/polkit-1").exists())
            else:
                module = package.removesuffix("-dkms")
                source = destination / "usr/src" / f"{module}-{VERSION}"
                self.assertEqual((source / "VERSION").read_text().strip(), VERSION)
                if module == 'y720-cooling':
                    self.assertTrue((source / 'kernel/y720-thermal.h').is_file())
                    self.assertFalse((source / 'tests').exists())
                env = dict(os.environ, Y720_TEST_CONFIG=str(source / "dkms.conf"))
                result = subprocess.check_output(
                    ["bash", "-c", 'kernelver=$(uname -r); source "$Y720_TEST_CONFIG"; printf "%s" "$PACKAGE_VERSION"'],
                    env=env, text=True)
                self.assertEqual(result, VERSION)

    def test_dkms_build_and_install_failures_are_not_hidden(self):
        kernel = subprocess.check_output(["uname", "-r"], text=True).strip()
        if not (Path("/lib/modules") / kernel / "build").is_dir():
            self.skipTest("matching current headers required for postinst's read-only preflight")
        fake_bin = self.directory / "bin"
        fake_bin.mkdir()
        fake_dkms = fake_bin / "dkms"
        fake_dkms.write_text(
            '#!/bin/sh\nprintf "%s\\n" "$*" >> "$Y720_TEST_DKMS_LOG"\n'
            'if [ "$1" = "$Y720_TEST_DKMS_FAIL" ]; then exit 42; fi\nexit 0\n')
        fake_dkms.chmod(0o755)
        for package in ("y720-cooling-dkms", "y720-hid-dkms"):
            _, destination = self.extract(package, "all")
            log = self.directory / f"{package}.calls"
            for failure in ("build", "install"):
                log.write_text("")
                env = dict(os.environ, PATH=str(fake_bin) + os.pathsep + os.environ["PATH"],
                           Y720_TEST_DKMS_LOG=str(log), Y720_TEST_DKMS_FAIL=failure)
                result = subprocess.run(["sh", str(destination / "DEBIAN/postinst"), "configure"],
                                        env=env, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 42, result.stderr)
                calls = log.read_text().splitlines()
                self.assertTrue(any(call.startswith("build ") for call in calls))
                self.assertEqual(any(call.startswith("install ") for call in calls), failure == "install")


if __name__ == "__main__":
    unittest.main()
