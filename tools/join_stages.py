# SPDX-License-Identifier: MIT
"""Check the two stages and join them into the bootloader's payload."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from check_build import inspect

BASE, LIMIT = 0x1c02000, 0x1c0a800


def symbol(path, wanted):
    data = Path(path).read_bytes()
    header = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    sections = [struct.unpack_from('<IIIIIIIIII', data, header[6]+i*header[11]) for i in range(header[12])]
    for section in sections:
        if section[1] == 2:
            strings = sections[section[6]]
            table = data[strings[4]:strings[4]+strings[5]]
            for off in range(section[4], section[4]+section[5], section[9]):
                name, value = struct.unpack_from('<II', data, off)
                if table[name:].split(b'\0')[0].decode() == wanted:
                    return value
    raise ValueError(f'{path}: no {wanted}')


def stage0_code_hash(stage0):
    """The hash without the version, so it shows when stage 0's code really changed."""
    return hashlib.sha256(stage0[:0x50]+bytes(32)+stage0[0x70:]).hexdigest()


def join(stage0_elf, stage0_raw, stage1_elf, stage1_raw, out):
    first = inspect(Path(stage0_elf), Path(stage0_raw), BASE, LIMIT, 'boot_entry', True)
    origin = symbol(stage0_elf, 'stage1_entry')
    if origin != BASE+first['payload_bytes']:
        raise ValueError('stage 1 does not start where stage 0 ends')
    second = inspect(Path(stage1_elf), Path(stage1_raw), origin, LIMIT, 'boot_main')
    if symbol(stage1_elf, '__ram_end') > symbol(stage0_elf, '__stage0_limit'):
        raise ValueError('stage 1 runs into stage 0\'s data')
    payload = Path(stage0_raw).read_bytes()+Path(stage1_raw).read_bytes()
    if len(payload) > 0x3f2c or payload[0x40:0x48] != b'AC79BLID':
        raise ValueError('boot slot/identity violation')
    Path(out).write_bytes(payload)
    return {'payload_bytes': len(payload), 'private_ram_bytes': second['private_ram_bytes']+origin-BASE,
            'entry': first['entry'], 'sha256': hashlib.sha256(payload).hexdigest(),
            'stage0_bytes': first['payload_bytes'], 'stage0_code_sha256': stage0_code_hash(payload[:first['payload_bytes']]),
            'stage1_bytes': second['payload_bytes']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage0_elf')
    parser.add_argument('stage0_raw')
    parser.add_argument('stage1_elf')
    parser.add_argument('stage1_raw')
    parser.add_argument('out')
    args = parser.parse_args()
    print(json.dumps(join(args.stage0_elf, args.stage0_raw, args.stage1_elf, args.stage1_raw, args.out), sort_keys=True))


if __name__ == '__main__':
    main()
