#!/usr/bin/env python3
"""Replay the existing target TU command, redirecting source/object/deps privately.

Does not run make, prepare headers, build a kernel, or touch shared module objects.
The saved command and existing generated target headers are read-only inputs.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--saved-command', type=Path, default=ROOT / 'drivers/mediatek/ccci_util/.ccci_util_lib_fo.o.cmd')
p.add_argument('--source', type=Path, default=ROOT / 'drivers/mediatek/ccci_util/ccci_util_lib_fo.c')
p.add_argument('--output', type=Path, default=ROOT / 'out/tests/ccci-util-fo/aarch64')
a = p.parse_args()
a.output = a.output.resolve()
a.output.mkdir(parents=True, exist_ok=True)
line = a.saved_command.read_text().splitlines()[0]
assert line.startswith('savedcmd_ccci_util/ccci_util_lib_fo.o := ')
command = shlex.split(line.split(':=', 1)[1])
assert command[0] == 'aarch64-linux-gnu-gcc'
source = a.source.resolve()
obj = a.output / 'ccci_util_lib_fo.o'
assert command.count('ccci_util/ccci_util_lib_fo.c') == 1
assert command.count('-o') == 1
command[command.index('ccci_util/ccci_util_lib_fo.c')] = str(source)
command[command.index('-o') + 1] = str(obj)
dep = '-Wp,-MMD,ccci_util/.ccci_util_lib_fo.o.d'
assert command.count(dep) == 1
command[command.index(dep)] = '-Wp,-MMD,' + str(a.output / '.ccci_util_lib_fo.o.d')
(a.output / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
print('+', shlex.join(command), flush=True)
subprocess.run(command, cwd=a.output, check=True)
subprocess.run(['file', str(obj)], check=True)
subprocess.run(['aarch64-linux-gnu-readelf', '-h', str(obj)], check=True)
