# SPDX-License-Identifier: MIT
"""The addresses of sfx_pack and sfx_pack_end in a linked self-extracting installer, for sfx-layout check: the pack
src/sfx.c unpacks, as the linker placed it. Usage: sfx_symbols.py <llvm-nm> <sfx.elf>"""
import subprocess
import sys


def symbols(nm, elf):
    found = {}
    for line in subprocess.run([nm, '-P', elf], check=True, capture_output=True, text=True).stdout.splitlines():
        name, _, value = (line.split() + ['', '', ''])[:3]
        if name in ('sfx_pack', 'sfx_pack_end'):
            found[name] = '0x' + value
    if set(found) != {'sfx_pack', 'sfx_pack_end'}:
        sys.exit(f'{elf}: sfx_pack or sfx_pack_end missing')
    return [found['sfx_pack'], found['sfx_pack_end']]


if __name__ == '__main__':
    print(' '.join(symbols(sys.argv[1], sys.argv[2])))
