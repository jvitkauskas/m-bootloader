# SPDX-License-Identifier: MIT
"""Inspect own ELF32 products and enforce entry, initialized image and RAM limits."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess


def check_startup(instructions, symbols, base):
    """Verify the compiled A entry against the §4.1 CPU-state contract."""
    normal = symbols['boot_normal']
    if normal < base + 0x70:
        raise ValueError('entry continuation overlaps board identity')
    before = [text for address, text in instructions if base <= address < base+0x40]
    after = [text for address, text in instructions if normal <= address < symbols['__payload_end']]

    def plain(text):
        return ' '.join(text.split('<', 1)[0].split())

    def load(text, value):
        match = re.fullmatch(r'r1 = (0x[0-9a-fA-F]+|[0-9]+)', plain(text))
        return bool(match) and int(match[1], 0) == value

    if (len(before) < 5 or not load(before[0], 0) or plain(before[1]) != 'rete = r1'
            or not load(before[2], normal) or plain(before[3]) != 'reti = r1'
            or plain(before[4]) != 'rti'):
        raise ValueError('§4.1: wrong exception return or stack/descriptor changed before rti')
    if (len(after) < 5 or not load(after[0], symbols['__exception_stack_top'])
            or plain(after[1]) != 'ssp = r1' or not load(after[2], symbols['__main_stack_top'])
            or plain(after[3]) != 'sp = r1' or not plain(after[4]).startswith('goto ')
            or not re.search(r'<boot_main(?:\s|[>:])', after[4])):
        raise ValueError('§4.1: wrong stack setup or C continuation after rti')


def inspect_startup(path, symbols, base):
    # Only disassemble this independently built ELF, never a reference binary.
    listing = subprocess.check_output(['toolchain/common/bin/objdump', '-d', str(path)], text=True)
    instructions = []
    for line in listing.splitlines():
        match = re.match(r'^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2}\s+)+(.+?)\s*$', line)
        if match:
            instructions.append((int(match[1], 16), match[2]))
    check_startup(instructions, symbols, base)


def inspect(path, raw_path, base, limit, entry, boot=False):
    data = path.read_bytes()
    header = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    if header[0][:6] != b'\x7fELF\x01\x01' or header[2] != 241 or header[4] != base:
        raise ValueError(f'{path}: wrong ELF target/entry')
    sections = [struct.unpack_from('<IIIIIIIIII', data, header[6]+i*header[11]) for i in range(header[12])]
    symbols = {}
    for section in sections:
        if section[1] == 2:
            strings = sections[section[6]]
            table = data[strings[4]:strings[4]+strings[5]]
            for off in range(section[4], section[4]+section[5], section[9]):
                name, value, _, _, _, _ = struct.unpack_from('<IIIBBH', data, off)
                symbols[table[name:].split(b'\0')[0].decode()] = value
        if section[2] & 2 and section[5]:
            if section[3] < base or section[3]+section[5] > limit:
                raise ValueError(f'{path}: allocated section outside private RAM')
    raw = raw_path.read_bytes()
    if symbols.get(entry) != base or symbols['__ram_end'] > limit:
        raise ValueError(f'{path}: callable entry or RAM limit violation')
    if len(raw) != symbols['__payload_end']-base:
        raise ValueError(f'{path}: initialized data absent or stray binary bytes')
    if boot and (len(raw) > 0x3820 or raw[0x40:0x48] != b'AC79BLID'):
        raise ValueError(f'{path}: boot slot/identity violation')
    if boot:
        inspect_startup(path, symbols, base)
    if not boot and len(raw) > 0x8000:
        raise ValueError(f'{path}: loader slot violation')
    return {'payload_bytes': len(raw), 'private_ram_bytes': symbols['__ram_end']-base,
            'entry': f'{base:08X}', 'sha256': hashlib.sha256(raw).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('raw', type=Path)
    parser.add_argument('base', type=lambda x: int(x, 0))
    parser.add_argument('limit', type=lambda x: int(x, 0))
    parser.add_argument('entry')
    parser.add_argument('--boot', action='store_true')
    args = parser.parse_args()
    print(json.dumps(inspect(args.elf, args.raw, args.base, args.limit, args.entry, args.boot), sort_keys=True))


if __name__ == '__main__':
    main()
