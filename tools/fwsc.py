# SPDX-License-Identifier: MIT
"""Build M-UPGRADE packages (.fwsc) that install this bootloader (docs/SPEC.md §5.6).

A package holds one file, ota.bin: the updater M-UPGRADE has the unit's
firmware store and the bootloader run. Ours is the installer (src/installer.c),
which writes only the bootloader. No M-VAVE firmware is in the package. Its
identity is the model and a version the unit does not have (M-UPGRADE refuses
an equal one); the unit keeps reporting its firmware's version afterwards.
"""
import argparse
from pathlib import Path
import re
import struct
from bankcb import crc16, enc, validate

BLOCK, KEEP, MIN_PREFIX, SENTINEL = 48, 47, 20, 0x7d
HEAD, ENTRY = 64, 80
UPDATER_LOAD, UPDATER_MAX, UPDATER_NAME = 0x01c0a800, 0x5b1c, b'usb_hid_ota.bin'
# Package variant: (bootloader board, model M-UPGRADE expects, identity prefix
# blocks as in M-VAVE's package). The Elite is an SMK-37 Pro by another name.
VARIANTS = {
    'fm1': ('fm1', 'FM-1', 20),
    'smk37-pro': ('smk37', 'SMK-37 Pro', 36),
    'smk37-elite': ('smk37', 'SMK-37 Elite', 20),
}
# As in M-VAVE's packages: header +10/+12 (meaning unknown) and chip name,
# first file at 0x400, files 32-byte aligned.
HEAD_FIELDS, CHIP, FIRST = (4, 0x200), b'AC791N', 0x400
OTA_TYPE = 0x64


def unweave(raw):
    """(identity, prefix block count, image without the identity bytes)."""
    if len(raw) < 0x1000:
        raise ValueError('not a firmware package (.fwsc)')
    prefix = 0
    while prefix < MIN_PREFIX or raw[prefix * BLOCK + KEEP] == SENTINEL:
        prefix += 1
        if (prefix + 1) * BLOCK > len(raw):
            raise ValueError('not a firmware package (.fwsc)')
    identity = bytearray()
    for i in range(prefix):
        marker = raw[i * BLOCK + KEEP]
        if marker == SENTINEL:
            break
        identity.append((marker - i - 1) & 255)
    image = b''.join(raw[i * BLOCK:i * BLOCK + KEEP] for i in range(prefix)) + raw[prefix * BLOCK:]
    return identity.decode('ascii'), prefix, image


def weave(identity, prefix, image):
    name = identity.encode('ascii')
    if len(name) >= prefix:
        raise ValueError('identity does not fit the prefix blocks')
    markers = [(c + i + 1) & 255 for i, c in enumerate(name)]
    if SENTINEL in markers:
        raise ValueError('identity would encode a sentinel byte')
    markers += [SENTINEL] * (prefix - len(name))
    out = b''.join(image[i * KEEP:(i + 1) * KEEP] + bytes([m]) for i, m in enumerate(markers))
    out += image[prefix * KEEP:]
    # The host keeps unweaving while the next block's marker is the sentinel.
    if out[prefix * BLOCK + KEEP] == SENTINEL:
        raise ValueError('image byte after the prefix reads as a sentinel')
    return out


def entries(image):
    """The file list, each entry with its decoded fields and image position."""
    head = enc(image[:HEAD])
    if crc16(head[2:]) != struct.unpack_from('<H', head)[0]:
        raise ValueError('package header CRC mismatch')
    list_crc, total, count = struct.unpack_from('<HIH', head, 2)
    if total != len(image) or HEAD + ENTRY * count > len(image):
        raise ValueError('package size or file count mismatch')
    if crc16(image[HEAD:HEAD + ENTRY * count]) != list_crc:
        raise ValueError('package file list CRC mismatch')
    result = []
    for i in range(count):
        at = HEAD + ENTRY * i
        entry = enc(image[at:at + ENTRY])
        kind, _, crc, offset, size = struct.unpack_from('<HHHxxII', entry)
        if offset + size > len(image):
            raise ValueError('package member outside the image')
        result.append({'at': at, 'type': kind, 'crc': crc, 'offset': offset, 'size': size,
                       'name': entry[64:].split(b'\0', 1)[0].decode('ascii', 'replace')})
    return result


def lz4_block(data):
    """One LZ4 block, greedy, without references outside it."""
    out, table, anchor, at = bytearray(), {}, 0, 0

    def length(n):
        while n >= 255:
            out.append(255)
            n -= 255
        out.append(n)

    def sequence(literals, match=None):
        token = min(len(literals), 15) << 4
        if match:
            token |= min(match[1] - 4, 15)
        out.append(token)
        if len(literals) >= 15:
            length(len(literals) - 15)
        out.extend(literals)
        if match:
            out.extend(struct.pack('<H', match[0]))
            if match[1] - 4 >= 15:
                length(match[1] - 19)

    # The format ends with at least five literals; the last match starts 12 before the end.
    while at < len(data) - 12:
        key = data[at:at + 4]
        seen = table.get(key)
        table[key] = at
        if seen is None or at - seen > 0xffff:
            at += 1
            continue
        n = 4
        while at + n < len(data) - 5 and data[seen + n] == data[at + n]:
            n += 1
        sequence(data[anchor:at], (at - seen, n))
        at += n
        anchor = at
    sequence(data[anchor:])
    return bytes(out)


def lz4_unblock(data, size, history=b''):
    """One LZ4 block; matches may reach into history, the output before it."""
    out, at = bytearray(history), 0
    while True:
        token = data[at]
        at += 1

        def length(n):
            nonlocal at
            if n == 15:
                while True:
                    n += data[at]
                    at += 1
                    if data[at - 1] != 255:
                        break
            return n
        n = length(token >> 4)
        out += data[at:at + n]
        at += n
        if at == len(data):
            break
        distance = struct.unpack_from('<H', data, at)[0]
        at += 2
        if not 0 < distance <= len(out):
            raise ValueError('LZ4 match before the block')
        for _ in range(length(token & 15) + 4):
            out.append(out[-distance])
    if len(out) != len(history) + size:
        raise ValueError('LZ4 block size mismatch')
    return bytes(out[len(history):])


def file_record(name, address, data):
    record = bytearray(32)
    struct.pack_into('<HII', record, 2, crc16(data), address, len(data))
    record[12] = 0x41
    record[16:32] = name.ljust(16, b'\0')
    struct.pack_into('<H', record, 0, crc16(record[2:]))
    return bytes(record)


def updater_stream(code):
    """A staged updater as ota.bin carries it (§6.3): file record, compression
    header, then LZ4 blocks of 4 KiB output, each at most 4 KiB compressed."""
    if not code or len(code) > UPDATER_MAX:
        raise ValueError('installer exceeds the stock updater size')
    blocks = b''
    for at in range(0, len(code), 0x1000):
        part = code[at:at + 0x1000]
        packed = lz4_block(part)
        if len(packed) > 0x1000:
            raise ValueError('installer block does not compress to 4 KiB')
        blocks += struct.pack('<II', len(packed), len(part)) + packed
    head = bytearray(32)
    struct.pack_into('<HIII', head, 2, crc16(code), len(code), UPDATER_LOAD, 0)
    head[16:32] = UPDATER_NAME.ljust(16, b'\0')
    struct.pack_into('<H', head, 0, crc16(head[2:]))
    body = bytes(head) + blocks
    return file_record(UPDATER_NAME, 32, body) + body


def updater_code(stream):
    """The code in a staged updater stream, checked as a bootloader checks it."""
    record, head = stream[:32], stream[32:64]
    if crc16(record[2:32]) != struct.unpack_from('<H', record)[0] or crc16(head[2:]) != struct.unpack_from('<H', head)[0]:
        raise ValueError('updater record or header CRC mismatch')
    crc, size, load = struct.unpack_from('<HII', head, 2)
    if load != UPDATER_LOAD or not 0 < size <= UPDATER_MAX:
        raise ValueError('updater load address or size not the stock updater\'s')
    code, at = b'', 64
    while at < len(stream):
        packed, part = struct.unpack_from('<II', stream, at)
        if not 0 < packed <= 0x1000 or at + 8 + packed > len(stream):
            raise ValueError('updater block out of bounds')
        code += lz4_unblock(stream[at + 8:at + 8 + packed], part, code)
        at += 8 + packed
    if len(code) != size or crc16(code) != crc:
        raise ValueError('updater code CRC mismatch')
    return code


def package(variant, container, installer, version='099'):
    """The package for variant: identity, header, and ota.bin with the installer."""
    board, model, prefix = VARIANTS[variant]
    if validate(container) != board or installer.count(container) != 1:
        raise ValueError('installer does not carry this board\'s bootloader')
    if not re.fullmatch(r'[0-9]{3}', version):
        raise ValueError('version must be three digits')
    stream = updater_stream(installer)
    allocated = -(-len(stream) // 32) * 32
    entry = bytearray(ENTRY)
    struct.pack_into('<HHHxxIII', entry, 0, OTA_TYPE, 0, crc16(stream), FIRST, len(stream), allocated)
    entry[64:] = b'ota.bin'.ljust(16, b'\0')
    head = bytearray(HEAD)
    struct.pack_into('<HIHHI', head, 2, crc16(enc(entry)), FIRST + allocated, 1, *HEAD_FIELDS)
    head[16:16 + len(CHIP)] = CHIP
    struct.pack_into('<H', head, 0, crc16(head[2:]))
    image = enc(head) + enc(entry)
    image += b'\xff' * (FIRST - len(image)) + stream + b'\xff' * (allocated - len(stream))
    identity = f'{model}_{version}'
    out = weave(identity, prefix, image)
    if installed(out) != (identity, installer):
        raise ValueError('package does not read back')
    return out


def installed(raw):
    """(identity, updater code) of a package, every check on the way."""
    identity, _, image = unweave(raw)
    found = [e for e in entries(image) if e['type'] == OTA_TYPE and e['name'] == 'ota.bin']
    if len(found) != 1:
        raise ValueError('package has no single ota.bin')
    stream = image[found[0]['offset']:found[0]['offset'] + found[0]['size']]
    if crc16(stream) != found[0]['crc']:
        raise ValueError('ota.bin CRC mismatch')
    return identity, updater_code(stream)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    sub = parser.add_subparsers(dest='op', required=True)
    build = sub.add_parser('build')
    build.add_argument('--version', default='099')
    build.add_argument('variant', choices=sorted(VARIANTS))
    build.add_argument('container', type=Path)
    build.add_argument('installer', type=Path)
    build.add_argument('output', type=Path)
    show = sub.add_parser('info')
    show.add_argument('package', type=Path)
    args = parser.parse_args()
    if args.op == 'build':
        args.output.write_bytes(package(args.variant, args.container.read_bytes(), args.installer.read_bytes(),
                                        args.version))
    else:
        identity, prefix, image = unweave(args.package.read_bytes())
        print(f'{identity} ({prefix} prefix blocks)')
        for e in entries(image):
            print(f"  {e['name']:<16} type {e['type']:#04x} {e['size']:>8} bytes CRC {e['crc']:04X}")


if __name__ == '__main__':
    main()
