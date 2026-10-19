# SPDX-License-Identifier: MIT
"""Check the M-UPGRADE packages and the installer's C against the Python tools,
and (when present) our package conventions against M-VAVE's packages."""
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from bankcb import crc16, enc, install
from fwsc import (CHIP, FIRST, HEAD, HEAD_FIELDS, OTA_TYPE, VARIANTS, entries, installed, lz4_block,
                  lz4_unblock, package, unweave, updater_code, updater_stream, weave)

HOST = Path('build/installer-host')


def refuses(fn, *args):
    try:
        fn(*args)
    except (ValueError, struct.error):
        return
    raise AssertionError('malformed input accepted')


def file_record(name, address, length, attribute=0):
    record = bytearray(32)
    struct.pack_into('<HII', record, 2, 0xffff, address, length)
    record[12] = attribute
    record[16:32] = name.ljust(16, b'\0')
    struct.pack_into('<H', record, 0, crc16(record[2:]))
    return enc(record)


def synthetic_flash():
    # Public format facts only; the patterned data detects unrelated writes.
    flash = bytearray(range(256)) * 256
    head = bytearray(32)
    struct.pack_into('<I', head, 8, 0xff000)
    head[12:14] = bytes((0x10, 16))
    struct.pack_into('<H', head, 0, crc16(head[2:]))
    flash[:32] = enc(head)
    flash[32:64] = file_record(b'uboot.boot', 0xa0, 0x3830)
    flash[64:96] = file_record(b'isd_config.ini', 0x38d0, 16, 2)
    flash[96:128] = file_record(b'app_dir_head', 0x4000, 0xffffffff, 0x81)
    flash[128:160] = file_record(b'key_mac', 0xff000, 0x1000, 0x12)
    return bytes(flash)


def host(op, *inputs):
    """Run the chip's C on the host; None if it refuses."""
    with tempfile.TemporaryDirectory() as temp:
        paths = []
        for i, data in enumerate(inputs):
            if len(data) < 32:  # a number, not a file
                paths.append(data.decode())
                continue
            paths.append(Path(temp, f'in{i}'))
            paths[-1].write_bytes(data)
        result = subprocess.run([HOST, op, *paths, Path(temp, 'out')])
        assert result.returncode in (0, 1)
        return Path(temp, 'out').read_bytes() if result.returncode == 0 else None


def boot_area_variants():
    """A unit image, then layouts the boot-area rewrite has to refuse."""
    image = bytearray(range(256)) * 4096
    image[:0x10000] = synthetic_flash()
    yield bytes(image)
    for slot, name, address, length, attribute in (
            (64, b'isd_config.ini', 0x2000, 16, 2), (64, b'isd_config.ini', 0x38c8, 16, 2),
            (64, b'isd_config.ini', 0x20, 16, 2), (96, b'app_dir_head', 0x2000, 0xffffffff, 0x81),
            (96, b'app_dir_head', 0x90, 0xffffffff, 0x81), (96, b'app_dir_head', 0x4000, 0x2000, 0x81),
            (96, b'app_dir_head', 0x4000, 0xff000, 0x81), (128, b'key_mac', 0x3000, 0x1000, 0x12),
            (128, b'other_record', 0xffff0, 0x20, 0x12), (128, b'isd_config.ini', 0x38d0, 16, 2),
            (32, b'uboot.boot', 0x2000, 0x1000, 0), (32, b'uboot.boot', 0xa0, 0x3831, 0),
            (32, b'uboot.bin', 0xa0, 0x3830, 0)):
        bad = bytearray(image)
        bad[slot:slot + 32] = file_record(name, address, length, attribute)
        yield bytes(bad)
    for at in (0, 32, 64, 96, 128):
        bad = bytearray(image)
        bad[at] ^= 1
        yield bytes(bad)


def staged_request(image, record_at, stream_at, stream):
    """image with stream staged as the firmware stages it, and its flash request."""
    out = bytearray(image)
    out[stream_at:stream_at + len(stream)] = stream
    outer = bytearray(stream[:32])
    struct.pack_into('<II', outer, 4, stream_at, len(stream))
    struct.pack_into('<H', outer, 0, crc16(outer[2:]))
    out[stream_at:stream_at + 32] = outer
    request = bytearray(80)
    struct.pack_into('<HHH', request, 2, 0x5a0d, 0x5a01, 0x5441)
    request[8:20] = b'ota-FM-1_015'
    struct.pack_into('<I', request, 72, stream_at)
    struct.pack_into('<H', request, 0, crc16(request[2:]))
    out[record_at:record_at + 80] = request
    return bytes(out)


def python_install(image, container):
    try:
        return install(image, container)
    except (ValueError, struct.error):
        return None


def members(image):
    return {e['name']: (e['type'], image[e['offset']:e['offset'] + e['size']]) for e in entries(image)}


containers = {board: Path('build', board, 'uboot.boot').read_bytes() for board in ('fm1', 'smk37')}
installers = {board: Path('build', board, 'installer.bin').read_bytes() for board in ('fm1', 'smk37')}
rng = random.Random(1)
for sample in (b'', b'x', bytes(12), bytes(13), bytes(4096), bytes(rng.randrange(4) for _ in range(4096)),
               bytes(rng.randrange(256) for _ in range(300)), b'abcd' * 1000 + b'tail!'):
    assert lz4_unblock(lz4_block(sample), len(sample)) == sample
for board, container in containers.items():
    installer = installers[board]
    assert installer.count(container) == 1
    stream = updater_stream(installer)
    assert updater_code(stream) == installer and host('decode', stream) == installer
    refuses(updater_stream, installer + bytes(0x5b1c))
    bad = bytearray(stream)
    bad[100] ^= 1
    assert host('decode', bytes(bad)) is None
    accepted = []
    for i, image in enumerate(boot_area_variants()):
        expected = python_install(image, container)
        assert host('install', image, container) == expected
        if expected is not None:
            accepted.append(i)
    assert accepted == [0, 7]  # the unit image, and one with a bank-info record at FF000
    # The installer retires the flash copy of its own request, and only that.
    image = next(boot_area_variants())
    staged = staged_request(image, 0xe7f00, 0xe3000, stream)
    staged = staged_request(staged, 0xc7f00, 0xc0000, updater_stream(installer[:-4]))
    end = 0xe3000 + len(stream)
    retired = host('retire', staged, str(end).encode())
    assert retired is not None and [i for i in range(len(staged)) if staged[i] != retired[i]] == [0xe7f00, 0xe7f01]
    assert retired[0xe7f00:0xe7f02] == b'\0\0'
    assert host('retire', retired, str(end).encode()) is None
    assert host('retire', staged, str(end + 1).encode()) is None
    for at in (0, 2, 12, 14, 16, len(container) - 1):
        bad = bytearray(container)
        bad[at] ^= 1
        assert host('install', image, bytes(bad)) is None
BUILT = {'fm1': ('build/fm1/uboot.fwsc', 'FM-1_099'),
         'smk37-pro': ('build/smk37/uboot-pro.fwsc', 'SMK-37 Pro_099'),
         'smk37-elite': ('build/smk37/uboot-elite.fwsc', 'SMK-37 Elite_099')}
assert set(BUILT) == set(VARIANTS)
for variant, (path, identity) in BUILT.items():
    board = VARIANTS[variant][0]
    other = 'smk37' if board == 'fm1' else 'fm1'
    built = Path(path).read_bytes()
    assert built == package(variant, containers[board], installers[board])
    assert installed(built) == (identity, installers[board]) and len(built) < 0x6000
    found, prefix, image = unweave(built)
    assert prefix == VARIANTS[variant][2] and weave(found, prefix, image) == built
    listed = members(image)
    assert list(listed) == ['ota.bin'] and listed['ota.bin'][0] == OTA_TYPE
    assert host('decode', listed['ota.bin'][1]) == installers[board]
    assert unweave(package(variant, containers[board], installers[board], '042'))[0] == identity[:-3] + '042'
    refuses(package, variant, containers[other], installers[board])
    refuses(package, variant, containers[board], installers[other])
    for version in ('99', '0999', '09a', ''):
        refuses(package, variant, containers[board], installers[board], version)
    for at in (0, 2, HEAD + 4, HEAD + 12, FIRST + 40, FIRST + 100):
        bad = bytearray(image)
        bad[at] ^= 1
        refuses(installed, weave(found, prefix, bytes(bad)))
refuses(unweave, b'\x7d' * 0x2000)
refuses(weave, 'A' * 20, 20, bytes(0x2000))
refuses(weave, '|', 20, bytes(0x2000))  # '|' + 1 is the sentinel

# The conventions copied from M-VAVE's packages, where those are at hand.
vendor = 0
for name, variant in (('fm1-015.fwsc', 'fm1'), ('smk37-016.fwsc', 'smk37-pro'),
                      ('smk37elite-016.fwsc', 'smk37-elite')):
    try:
        raw = Path('testdata', name).read_bytes()
    except FileNotFoundError:
        print(f'SKIP: {name} not present (package conventions)')
        continue
    identity, prefix, image = unweave(raw)
    head = enc(image[:HEAD])
    listed = entries(image)
    assert identity.rpartition('_')[0] == VARIANTS[variant][1]
    assert prefix == VARIANTS[variant][2] and weave(*unweave(raw)) == raw
    assert struct.unpack_from('<HI', head, 10) == HEAD_FIELDS and head[16:16 + len(CHIP)] == CHIP
    assert min(e['offset'] for e in listed) == FIRST
    ota = [e for e in listed if e['type'] == OTA_TYPE]
    assert [e['name'] for e in ota] == ['ota.bin']
    stream = image[ota[0]['offset']:ota[0]['offset'] + ota[0]['size']]
    assert host('decode', stream) == updater_code(stream)
    vendor += 1
print('PASS: LZ4, installer stream through the C decoder, C boot-area rewrite equal to bankcb, '
      f'flash request retirement, {len(BUILT)} packages, {vendor} M-VAVE package convention checks')
