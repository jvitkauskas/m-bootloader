# SPDX-License-Identifier: MIT
"""Independent plain BANKCB validation and same-image boot-slot replacement."""
import argparse
from pathlib import Path
import struct


# What's left of the configuration: the chip-key record, and the settings 1.3 and
# older require, so that they start if installed over this version (SPEC section 5.3).
CONFIG_KEY = 34
CONFIG_SETTINGS = b''.join(bytes([4]) + name + b'\0' + value for name, value in (
    (b'OSC', b'OSC0'),
    (b'OSC_FREQ', struct.pack('<I', 24000000)),
    (b'SYS_CLK', struct.pack('<I', 360000000)),
    (b'HSB_DIV', struct.pack('<I', 1)),
    (b'LSB_DIV', struct.pack('<I', 2)),
))
CONFIG_AT = 0x4000 - CONFIG_KEY - len(CONFIG_SETTINGS)


def crc16(data, state=0):
    for byte in data:
        state ^= byte << 8
        for _ in range(8):
            state = ((state << 1) ^ (0x1021 if state & 0x8000 else 0)) & 65535
    return state


def enc(data, key=65535):
    out = bytearray(data)
    for i in range(len(out)):
        out[i] ^= key & 255
        key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 65535
    return bytes(out)


def validate(container, board=None):
    if len(container) < 16:
        raise ValueError('truncated BANKCB')
    banks, length, load, offset, payload_crc, header_crc = struct.unpack_from('<HHIIHH', container)
    if (banks, load, offset) != (1, 0x01c02000, 16):
        raise ValueError('unsupported BANKCB geometry')
    if len(container) != length + 16 or not 0x70 <= length <= 0x3ef0:
        raise ValueError('payload size or stock flash slot violation')
    if crc16(container[:14]) != header_crc or crc16(container[16:]) != payload_crc:
        raise ValueError('BANKCB CRC mismatch')
    identity = container[0x50:0x80]
    if identity[:8] != b'AC79BLID' or b'\0' not in identity[16:]:
        raise ValueError('missing boot identity or unterminated version')
    tag = identity[8:16]
    if tag not in (b'fm1\0\0\0\0\0', b'smk37\0\0\0'):
        raise ValueError('unknown board identity')
    found = tag.split(b'\0', 1)[0].decode('ascii')
    if board is not None and found != board:
        raise ValueError('wrong board')
    return found


def pack(payload, board):
    if len(payload) > 0x3ef0:
        raise ValueError('payload exceeds 16,172-byte slot')
    head = struct.pack('<HHIIH', 1, len(payload), 0x01c02000, 16, crc16(payload))
    result = head + struct.pack('<H', crc16(head)) + payload
    validate(result, board)
    return result


def install(image, container):
    validate(container)
    if len(image) != 0x100000:
        raise ValueError('requires a complete 1 MiB unit image')
    head, record = enc(image[:32]), bytearray(enc(image[32:64]))
    if crc16(head[2:]) != struct.unpack_from('<H', head)[0]:
        raise ValueError('invalid flash header')
    if crc16(record[2:]) != struct.unpack_from('<H', record)[0] or record[16:32].split(b'\0')[0] != b'uboot.boot':
        raise ValueError('invalid boot directory record')
    address, length = struct.unpack_from('<II', record, 4)
    if address != 0xa0 or not 16 <= length <= CONFIG_AT-0xa0:
        raise ValueError('unsupported boot allocation')

    def preserve_allocation(address, length):
        if address >= len(image) or length > len(image) - address:
            raise ValueError('other boot-area allocation is outside the image')
        if address < 0x4000 and address + length > 0xa0:
            raise ValueError('other boot-area allocation overlaps the rewritten area')

    # A0 is the first data allocation, so only these three more records fit.
    # Validate all of them: an unreadable record cannot establish a safe slot.
    config_slot = blob = None
    for offset in range(64, 0xa0, 32):
        other = enc(image[offset:offset+32])
        if crc16(other[2:]) != struct.unpack_from('<H', other)[0]:
            raise ValueError('invalid boot-area directory record')
        name = other[16:32].split(b'\0', 1)[0]
        address, length = struct.unpack_from('<II', other, 4)
        if name == b'isd_config.ini':
            # Only its chip-key record is kept, so that has to be good.
            if config_slot is not None or address < 0x38d0 or length < CONFIG_KEY or address + length > 0x4000:
                raise ValueError('invalid configuration allocation')
            blob = image[address:address+CONFIG_KEY] + CONFIG_SETTINGS
            if crc16(blob[:32]) != struct.unpack_from('<H', blob, 32)[0]:
                raise ValueError('invalid chip-key record')
            config_slot = offset
        elif name in (b'app_dir_head', b'app_dir_head2'):
            # Directory lengths are sentinels or bank-info addresses, not sizes.
            preserve_allocation(address, 32)
            if length not in (0, 0xffffffff):
                preserve_allocation(length, 32)
        else:
            preserve_allocation(address, 32 if length in (0, 0xffffffff) else length)
    if config_slot is None:
        raise ValueError('no configuration')
    out = bytearray(image)
    struct.pack_into('<HII', record, 2, crc16(container), 0xa0, len(container))
    struct.pack_into('<H', record, 0, crc16(record[2:]))
    out[32:64] = enc(record)
    config = bytearray(enc(image[config_slot:config_slot+32]))
    struct.pack_into('<HII', config, 2, crc16(blob), CONFIG_AT, len(blob))
    struct.pack_into('<H', config, 0, crc16(config[2:]))
    out[config_slot:config_slot+32] = enc(config)
    out[0xa0:0x4000] = b'\xff' * (0x4000-0xa0)
    out[0xa0:0xb0] = enc(container[:16])
    out[0xb0:0xa0+len(container)] = enc(container[16:])
    out[CONFIG_AT:0x4000] = blob
    return bytes(out)


def extract(image):
    record = enc(image[32:64])
    if crc16(record[2:]) != struct.unpack_from('<H', record)[0]:
        raise ValueError('invalid record CRC')
    address, size = struct.unpack_from('<II', record, 4)
    if address != 0xa0 or size < 16 or size > 0x3f3c or address+size > len(image):
        raise ValueError('invalid boot record geometry')
    container = enc(image[address:address+16]) + enc(image[address+16:address+size])
    validate(container)
    if crc16(container) != struct.unpack_from('<H', record, 2)[0]:
        raise ValueError('boot file CRC mismatch')
    return container


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='op', required=True)
    wrap = sub.add_parser('wrap')
    wrap.add_argument('board', choices=['fm1', 'smk37'])
    wrap.add_argument('payload', type=Path)
    wrap.add_argument('output', type=Path)
    check = sub.add_parser('check')
    check.add_argument('container', type=Path)
    args = parser.parse_args()
    if args.op == 'wrap':
        args.output.write_bytes(pack(args.payload.read_bytes(), args.board))
    else:
        print(validate(args.container.read_bytes()))


if __name__ == '__main__':
    main()
