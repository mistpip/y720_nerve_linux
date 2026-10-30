#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Release invariants and installation tests; no real hardware or system writes."""
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Release(unittest.TestCase):
    def test_versions_are_consistent(self):
        self.assertEqual((ROOT/'VERSION').read_text().strip(),'0.1.1')
        for source in ['fan/kernel/y720_cooling.c','keyboard/kernel/y720_hid.c','compat/gamezone/lenovo-wmi-gamezone.c']:
            self.assertIn('MODULE_VERSION(Y720_VERSION)',(ROOT/source).read_text())
        for config in ['fan/dkms.conf','keyboard/dkms.conf']:
            self.assertIn('PACKAGE_VERSION=$(cat ',(ROOT/config).read_text())
            self.assertIn('AUTOINSTALL="no"',(ROOT/config).read_text())
        self.assertIn('#define CLI_VERSION Y720_VERSION',(ROOT/'userspace/common/cli.h').read_text())
        self.assertNotIn('Y720_TEST_FAULTS',(ROOT/'fan/kernel/y720-native.h').read_text())
        for script in ['install.sh','uninstall.sh','scripts/keyboard-bind.sh']:
            result = subprocess.run(['bash',str(ROOT/script),'--version'],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(result.stdout.strip(),'0.1.1')

    def test_no_research_payload_or_binary_sources(self):
        # Generated files are ignored by Git. Restrict the publishable source tree.
        forbidden = {'evidence','research','outputs','packages','history','mirror-backup'}
        for path in ROOT.rglob('*'):
            relative = path.relative_to(ROOT)
            if any(part in {'build','__pycache__','.git'} for part in relative.parts): continue
            if path.name in {'Module.symvers','modules.order'} or path.name.startswith('.') and path.name.endswith('.cmd'): continue
            if path.name.endswith(('.o','.ko','.mod','.mod.c','.mod.cmd')): continue
            self.assertFalse(forbidden.intersection(relative.parts),str(relative))
            if path.is_file():
                self.assertNotIn(path.suffix,{'.rar','.exe','.rom','.bin','.zip','.log','.pem','.key'},str(relative))
                self.assertFalse(path.read_bytes().startswith(b'\x7fELF'),str(relative))

    def test_root_cli_install_and_preflight(self):
        with tempfile.TemporaryDirectory() as temporary:
            prefix = Path(temporary)/'CLI with spaces'
            install = ['bash',str(ROOT/'install.sh'),'--cli','--prefix',str(prefix)]
            uninstall = ['bash',str(ROOT/'uninstall.sh'),'--cli','--prefix',str(prefix)]
            subprocess.run(install,check=True,capture_output=True)
            subprocess.run(install,check=True,capture_output=True)
            for name in ['y720-fan','y720-backlight']:
                self.assertTrue((prefix/'bin'/name).read_bytes().startswith(b'\x7fELF'))
            self.assertFalse((prefix/'libexec').exists())
            foreign = prefix/'share/doc/y720-cli/CLI.md'
            original = foreign.read_bytes()
            foreign.write_bytes(b'foreign modification')
            self.assertNotEqual(subprocess.run(uninstall,capture_output=True).returncode,0)
            self.assertTrue((prefix/'bin/y720-fan').exists())
            foreign.write_bytes(original)
            subprocess.run(uninstall,check=True,capture_output=True)
            self.assertFalse((prefix/'bin/y720-fan').exists())
            self.assertFalse((prefix/'libexec').exists())
            self.assertFalse(foreign.exists())

    def test_bad_install_arguments_never_mutate(self):
        for script in ['install.sh','uninstall.sh']:
            for args in [['--unknown'],['--prefix','relative'],['--prefix','/']]:
                result = subprocess.run(['bash',str(ROOT/script),*args],capture_output=True)
                self.assertEqual(result.returncode,2,(script,args,result.stderr))
        for kernel in ['../bad','', '7.0/bad']:
            result = subprocess.run(['bash',str(ROOT/'install.sh'),'--drivers','--kernel',kernel],capture_output=True)
            self.assertEqual(result.returncode,2,result.stderr)

    def test_flight_mode_is_explicit_opt_in(self):
        source = (ROOT/'keyboard/kernel/y720_hid.c').read_text()
        self.assertRegex(source, r'static bool radio_zero_pulse;')
        self.assertIn('module_param(radio_zero_pulse, bool, 0400)', source)
        self.assertIn('MODULE_DEVICE_TABLE(hid, y720_ids)', source)
        self.assertIn('radio_zero_pulse=1', (ROOT/'docs/DEVELOPMENT.md').read_text())
        for name in ['install.sh', 'uninstall.sh']:
            self.assertIn('CLI.md DEVELOPMENT.md', (ROOT/'userspace'/name).read_text())

    def test_documentation_is_consolidated(self):
        self.assertEqual({p.name for p in (ROOT/'docs').glob('*.md')}, {'CLI.md','DEVELOPMENT.md'})
        self.assertIn('y720-thermal.h', (ROOT/'scripts/install-dkms.sh').read_text())
        self.assertNotIn('y720_thermal_selftest', (ROOT/'scripts/install-dkms.sh').read_text())

    def test_curve_has_one_kernel_temperature_and_no_bridge(self):
        native = (ROOT/'fan/kernel/y720-native.h').read_text()
        driver = (ROOT/'fan/kernel/y720_cooling.c').read_text()
        self.assertIn('thermal_zone_get_zone_by_name("x86_pkg_temp")', native)
        self.assertIn('y720_control_temperature(&temperature)', native)
        for forbidden in ['NVML', 'pm_runtime_', 'pci_get_', 'gpu_core_sample', 'gpu_state_show']:
            self.assertNotIn(forbidden, native + driver)
        self.assertIn('temperature_source', driver)
        self.assertNotIn('libexec', (ROOT/'scripts/build-debs.sh').read_text())


if __name__ == '__main__': unittest.main()
