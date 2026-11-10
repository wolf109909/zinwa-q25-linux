#!/usr/bin/env python3
"""Compile verbatim production parser functions with checked host MMIO mocks."""
import argparse
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source', type=Path, default=ROOT / 'drivers/mediatek/ccci_util/ccci_util_lib_fo.c')
p.add_argument('--output', type=Path, default=ROOT / 'out/tests/ccci-util-fo/host')
p.add_argument('--cc', default=os.environ.get('CC', 'clang'))
p.add_argument('--fuzz', type=int, default=50000)
p.add_argument('--signed-char', action='store_true')
p.add_argument('--debug', action='store_true')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
source = a.source.read_text()


def section(start, end):
    return source[source.index(start):source.index(end, source.index(start))]


# No rewritten parser: each function and its on-wire types/globals comes directly
# from the source being tested. Keep unrelated kernel APIs out of the host TU.
parts = [
    section('#define MAX_LK_INFO_SIZE', '/*====================================================== */\n/* Feature option setting'),
    section('#define CCCI_FO_VER_02', 'struct _smem_layout'),
    section('static void md_mem_info_parsing(void)', '/*---- Modem check header section'),
    section('static void verify_md_enable_setting(void)', 'static struct _mpu_cfg *s_g_md_mpu_cfg_list;'),
]
header = (ROOT / 'drivers/mediatek/ccci_util/ccci_util_lib_main.h').read_text()
max_md = re.search(r'^#define MAX_MD_NUM_AT_LK\s+.*$', header, re.MULTILINE).group()
(a.output / 'production-parser.inc').write_text(max_md + '\n' + '\n'.join(parts))
exe = a.output / 'ccci_util_lib_fo_test'
command = [a.cc, '-std=gnu11', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
           '-Wno-unused-variable', '-Wno-sign-compare', '-Wno-unused-function',
           '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
           '-fsigned-char' if a.signed_char else '-funsigned-char',
           '-I', str(a.output), str(ROOT / 'tests/ccci_util_lib_fo_test.c'), '-o', str(exe)]
if a.debug:
    command.insert(1, '-DLK_LOAD_MD_INFO_DEBUG_EN')
print('+', ' '.join(command), flush=True)
subprocess.run(command, check=True)
print('+', exe, a.fuzz, flush=True)
subprocess.run([str(exe), str(a.fuzz)], check=True)
