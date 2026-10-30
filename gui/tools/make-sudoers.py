#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build-time exact argv allowlist; no shell or privileged profile paths."""
import sys
from pathlib import Path

bindir, output = sys.argv[1:]
if not bindir.startswith('/') or any(c in bindir for c in '\n\r\\'):
    raise SystemExit('Invalid absolute binary directory')

def escape(value):
    return ''.join('\\' + c if c in ' \t,:=#!@' else c for c in value)

fan = escape(bindir.rstrip('/') + '/y720-fan')
light = escape(bindir.rstrip('/') + '/y720-backlight')
commands = [f'{fan} --json status', f'{fan} --json mode auto',
            f'{fan} --json mode boost', f'{light} --json status', f'{light} --json apply -']
commands += [f'{fan} --json curve apply -', f'{fan} --json curve stop']
commands += [f'{fan} --json set --rpm {rpm}' for rpm in range(1500, 5101, 100)]
Path(output).write_text('# Generated Y720 GUI command allowlist; no arbitrary arguments.\n' +
                        ''.join(f'%sudo, %wheel ALL=(root) NOPASSWD: {cmd}\n' for cmd in commands))
