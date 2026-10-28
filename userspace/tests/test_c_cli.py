#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Product regression tests. All invocations are help/version/dry-run.
Only test installation uses a private temporary prefix. No device discovery.
"""
import json
import os
import signal
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIN = Path(os.environ.get('Y720_TEST_BIN', str(ROOT/'userspace/build')))


EFFECTS = dict(heartbeat=0, breath=1, smooth=2, static=3, wave=4)
COLORS = dict(red=0, **{'torch-red':1}, pink=2, magenta=3, purple=4, blue=6,
              cyan=9, green=12, yellow=15, orange=16, white=18, none=19)


def expected_profile(effect, colors, levels):
    return dict(schema_version=1,effect=effect,colors=[COLORS.get(v, v) for v in colors],brightness=levels)


def expected_packets(config):
    return [bytes([0xcc,0,EFFECTS[config['effect']],config['colors'][i],config['brightness'][i],i,0])
            for i in range(4)] + [bytes([0xcc,9,0,0,0,0,0])]


class CCLI(unittest.TestCase):
    def invoke(self, program, args, raw=None, expected=0):
        result = subprocess.run([str(BIN/f'y720-{program}'), '--json', '--dry-run', *args],
                                input=raw, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, expected, result.stderr)
        if expected == 0:
            self.assertEqual(result.stderr, b'')
            obj = json.loads(result.stdout)
            self.assertTrue(obj['ok'])
        else:
            self.assertEqual(result.stdout, b'')
            obj = json.loads(result.stderr)
            self.assertFalse(obj['ok'])
        self.assertEqual(obj['schema_version'], 1)
        self.assertEqual(obj['program'], f'y720-{program}')
        self.assertEqual(obj['version'], '0.1')
        return obj['data' if expected == 0 else 'error']

    def test_every_pwm_and_rpm(self):
        for kind, values in [('pwm', range(256)), ('rpm', range(1500, 5101))]:
            for value in values:
                data = self.invoke('fan', ['set', f'--{kind}', str(value)])
                rpm = (value+99)//100*100 if kind == 'rpm' else 1500+((value*36+254)//255)*100
                pwm = (rpm-1500)*255//3600 if kind == 'rpm' else value
                self.assertEqual(data['request'], dict(target_rpm=rpm,pwm=pwm,kind=kind,value=value))
                self.assertFalse(data['hardware_written'])
                self.assertFalse(data['device_verified'])

    def test_all_effects_colors_levels(self):
        for effect in EFFECTS:
            for color in range(20):
                for level in range(4):
                    config = expected_profile(effect, [color]*4, [level]*4)
                    data = self.invoke('backlight', ['set','--effect',effect,'--color',str(color),'--brightness',str(level)])
                    self.assertEqual(data['profile'], config)
                    self.assertEqual(data['reports_hex'], [packet.hex(' ') for packet in expected_packets(config)])

    def test_names_and_four_zone_profile(self):
        for name, value in COLORS.items():
            data = self.invoke('backlight', ['set', '--color', name])
            self.assertEqual(data['profile']['colors'], [value]*4)
        data = self.invoke('backlight', ['set','--colors','red','blue','green','white','--levels','0','1','2','3'])
        self.assertEqual(data['profile'], expected_profile('static',['red','blue','green','white'],[0,1,2,3]))
        off = self.invoke('backlight', ['off'])
        self.assertEqual(off['profile'], expected_profile('static',[0]*4,[0]*4))

    def test_profiles_valid_and_invalid(self):
        valid = dict(schema_version=1,effect='static',colors=['red','blue','green','white'],brightness=[0,1,2,3])
        for config in [valid, dict(valid, colors=[0,6,12,18]), dict(valid, colors=['00','06','12','18'])]:
            raw = json.dumps(config).encode()
            normalized = [COLORS[v] if v in COLORS else int(v) for v in config['colors']]
            self.assertEqual(self.invoke('backlight',['apply','-'],raw)['profile'], expected_profile(config['effect'],normalized,config['brightness']))
        raw = json.dumps(valid).replace('static', '\\u0073tatic').replace('effect','\\u0065ffect').encode()
        self.assertEqual(self.invoke('backlight',['apply','-'],raw)['profile']['effect'], 'static')
        base = json.dumps(valid).encode()
        self.invoke('backlight', ['apply','-'], b'\xef\xbb\xbf' + base)
        # Exact 4096-byte limit, no silent truncation at 4097.
        self.invoke('backlight', ['apply','-'], base + b' '*(4096-len(base)))
        malformed = [b'',b'[]',b'{}',b'null',b'{',b'\xff',b' '*4097,base+b'\0',base+b'{}',
                     base[:-1]+b',"effect":"static"}',base.replace(b'1,',b'1.0,',1),
                     base.replace(b'1,',b'true,',1),base.replace(b'"red"',b'true'),
                     base.replace(b'"red"',b'20'),base.replace(b'0, 1',b'false, 1'),
                     base.replace(b'0, 1',b'0.0, 1'),base.replace(b'0, 1',b'00, 1'),
                     base.replace(b'0, 1',b'0e0, 1'),base.replace(b'"red"',b'"\\u0000red"'),
                     base.replace(b'"red"',b'"\\ud800"'),base.replace(b'"red"',b'"\xc0\xaf"'),
                     base.replace(b'"static"',b'"sta\ntic"'),base[:-1]+b',}',
                     base.replace(b'schema_version',b'unknown'),base.replace(b'"static"',b'[]')]
        for config in [dict(valid,brightness=[1,2,3]),dict(valid,brightness=[1,2,3,0,1]),dict(valid,colors=[{}]*4),dict(valid,schema_version=2),dict(valid,unknown=1)]:
            malformed.append(json.dumps(config).encode())
        for raw in malformed:
            with self.subTest(raw=raw[:120]):
                self.assertEqual(self.invoke('backlight',['apply','-'],raw,2)['code'],'usage')
        for length in range(len(base)):
            self.invoke('backlight',['apply','-'],base[:length],2)

    def test_usage_boundaries(self):
        fan_bad = [[],['status'],['mode','curve'],['mode','auto','extra'],['set'],['set','--rpm','1499'],
                   ['set','--rpm','5101'],['set','--rpm','3000.0'],['set','--rpm','9223372036854775808'],
                   ['set','--pwm','-1'],['set','--pwm','256'],['set','--pwm','0','--rpm','1500'],
                   ['set','--pwm','1','--wait','nan'],['set','--pwm','1','--wait','inf'],
                   ['set','--pwm','1','--wait','0.9'],['set','--pwm','1','--wait','121'],
                   ['set','--pwm','1','--wait','0x1p1'],['set','--pwm','1','--tolerance','0'],
                   ['set','--pwm','1','--wait','2','--tolerance','501'],['--device','/tmp/hwmon1','set','--pwm','0']]
        light_bad = [[],['info'],['status'],['off','extra'],['set'],['apply'],['set','--color','20'],['set','--color','-1'],
                     ['set','--color','red','--colors','0','0','0','0'],['set','--color','red','--brightness','4'],
                     ['set','--color','red','--levels','0','0','0'],['set','--color','red','--effect','invalid'],
                     ['set','--color','red','--brightness','1','--levels','0','0','0','0'],['--device','../hidraw1','off']]
        for program, cases in [('fan',fan_bad),('backlight',light_bad)]:
            for args in cases:
                with self.subTest(program=program,args=args): self.invoke(program,args,expected=2)
        for mode in ['auto','boost']:
            self.assertEqual(self.invoke('fan',['mode',mode])['request'],dict(mode=mode))
        self.invoke('fan',['set','--pwm=0','--wait=1','--tolerance=0'])
        self.invoke('fan',['--device=hwmon123','set','--rpm=5100'])
        self.invoke('backlight',['--device=hidraw12','set','--color=red','--brightness=0'])

    def test_help_version_and_text(self):
        for program in ['fan','backlight']:
            for option in ['--help','--version']:
                result = subprocess.run([str(BIN/f'y720-{program}'),option],capture_output=True,timeout=5)
                self.assertEqual(result.returncode,0)
                self.assertEqual(result.stderr,b'')
                self.assertIn(b'0.1',result.stdout)
        result = subprocess.run([str(BIN/'y720-fan'),'--dry-run','mode','auto'],capture_output=True,text=True)
        self.assertEqual(result.returncode,0)
        self.assertIn('request={"mode":"auto"}',result.stdout)

    def test_cancel_blocked_stdin_and_closed_output(self):
        for sig in [signal.SIGINT,signal.SIGTERM]:
            with subprocess.Popen([str(BIN/'y720-backlight'),'--json','--dry-run','apply','-'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE) as process:
                # Wait for application signal handlers to be installed, without touching hardware.
                for _ in range(1000):
                    status = Path(f'/proc/{process.pid}/status').read_text()
                    caught = int(next(line.split()[1] for line in status.splitlines() if line.startswith('SigCgt:')),16)
                    if caught & (1 << (sig - 1)): break
                    __import__('time').sleep(.001)
                else: self.fail('signal handlers were not installed')
                process.send_signal(sig)
                out,err = process.communicate(timeout=5)
                self.assertEqual(process.returncode,128+sig)
                self.assertEqual(out,b'')
                self.assertEqual(json.loads(err)['error']['code'],'interrupted')
        reader, writer = os.pipe()
        os.close(reader)
        try:
            result = subprocess.run([str(BIN/'y720-fan'),'--json','--dry-run','mode','auto'],stdout=writer,stderr=subprocess.PIPE)
            self.assertEqual(result.returncode,141)
        finally: os.close(writer)

    def test_no_python_runtime_dependency(self):
        for program in ['fan','backlight']:
            executable = BIN/f'y720-{program}'
            self.assertTrue(executable.read_bytes().startswith(b'\x7fELF'))
            result = subprocess.run([str(executable),'--version'],env={'PATH':'/nonexistent'},capture_output=True)
            self.assertEqual(result.returncode,0)

    def test_installers_c_artifacts_and_symlinks(self):
        with tempfile.TemporaryDirectory() as temporary:
            prefix = Path(temporary)/'C prefix with spaces'
            binaries = prefix/'bin'
            binaries.mkdir(parents=True)
            foreign = Path(temporary)/'foreign'
            foreign.write_bytes(b'untouched')
            (binaries/'y720-fan').symlink_to(foreign)
            install = ['sh',str(ROOT/'userspace/install.sh'),str(prefix)]
            uninstall = ['sh',str(ROOT/'userspace/uninstall.sh'),str(prefix)]
            self.assertNotEqual(subprocess.run(install,capture_output=True).returncode,0)
            self.assertFalse((binaries/'y720-backlight').exists())
            self.assertEqual(foreign.read_bytes(),b'untouched')
            (binaries/'y720-fan').unlink()
            subprocess.run(install,check=True,capture_output=True)
            for name in ['y720-fan','y720-backlight']:
                self.assertTrue((binaries/name).read_bytes().startswith(b'\x7fELF'))
            document = prefix/'share/doc/y720-cli/C-DEVELOPMENT.md'
            original = document.read_bytes()
            document.unlink()
            document.symlink_to(foreign)
            self.assertNotEqual(subprocess.run(uninstall,capture_output=True).returncode,0)
            self.assertTrue((binaries/'y720-backlight').exists())
            self.assertEqual(foreign.read_bytes(),b'untouched')
            document.unlink()
            document.write_bytes(original)
            subprocess.run(uninstall,check=True,capture_output=True)
            self.assertFalse((binaries/'y720-fan').exists())
            self.assertFalse(document.exists())
            self.assertTrue(binaries.is_dir())

    def test_status_read_only_and_info_alias(self):
        # The fixture compiles the production command parser and discovery path
        # with syscall substitutions. Any open/ioctl/writer lock asserts fail.
        fixture = ROOT/'userspace/build/test-c-identity'
        states = []
        for command in ['status','info']:
            result = subprocess.run([str(fixture),'--json',command],capture_output=True,timeout=5)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(result.stderr,b'')
            obj = json.loads(result.stdout)
            self.assertEqual(obj['version'],'0.1')
            self.assertTrue(obj['ok'])
            states.append(obj['data'])
        self.assertEqual(states[0],states[1])
        self.assertEqual(states[0]['device'],'hidraw9')
        self.assertFalse(states[0]['state_known'])
        self.assertIsNone(states[0]['profile'])
        self.assertEqual(states[0]['driver'],'hid-generic')
        # Also exercise the real status routing in text mode and syntax failures.
        result = subprocess.run([str(fixture),'status'],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('profile=null',result.stdout)
        for args in [['--json','status','extra'],['--json','--dry-run','status']]:
            result = subprocess.run([str(fixture),*args],capture_output=True)
            self.assertEqual(result.returncode,2)
            self.assertEqual(result.stdout,b'')

    def test_fan_status_snapshots(self):
        fixture = ROOT/'userspace/build/test-c-fan'
        for mode in ['curve','readonly']:
            result = subprocess.run([str(fixture),mode],capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(result.stderr,b'')
            obj = json.loads(result.stdout)
            self.assertEqual(obj['version'],'0.1')
            self.assertEqual(obj['program'],'y720-fan')
            data = obj['data']
            self.assertEqual(data['fan_rpm'],[1500,1500])
            self.assertEqual(data['driver_version'],'0.1')
            self.assertEqual(data['controls_available'],mode == 'curve')
            if mode == 'readonly':
                for field in ['mode','mode_raw','target_rpm','pwm']: self.assertIsNone(data[field])
            else:
                self.assertEqual(data['mode'],'curve')
                self.assertEqual(data['target_rpm'],1500)


if __name__ == '__main__': unittest.main()
