# SPDX-License-Identifier: MIT
"""Verify newly built containers, fixture isolation, and host-compatible install."""
from pathlib import Path
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import zlib
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from bankcb import crc16, enc, extract, install, pack, validate
from check_build import inspect
from join_stages import join, stage0_code_hash
from pack_stage1 import call_filter, pack as pack_stage1
from version import boot_version

FIXTURES = {
    'fm1-stock.bin': ('1cfe35585ab2da776638d8eb13957d8f2d7817db4a7a194693c8992be58b22df', 0xf98496a0),
    'smk37-stock.bin': ('ed1e8533375457a08b0de23922993c314eae9f171bd4bd925ee0bba43bb7e0c4', 0x8e57bc96),
    'smk37-snoop37.bin': ('444088709abebfb6790011d405a87437ec4fb09b79589e1c47008083db7827c1', 0x6475373c),
    'smk37-baudgirl.bin': ('fedde66e70a85d5feaa74d2de4bdd8c6d137cabd1931b8e1cbc04c55631bb312', 0x1eabfb0a),
}


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


def synthetic_image():
    # Public format facts only; the patterned unit data detects unrelated writes.
    image = bytearray(range(256)) * 4096
    head = bytearray(32)
    struct.pack_into('<I', head, 8, 0xff000)
    head[12:14] = bytes((0x10, 16))
    struct.pack_into('<H', head, 0, crc16(head[2:]))
    image[:32] = enc(head)
    image[32:64] = file_record(b'uboot.boot', 0xa0, 0x3830)
    image[64:96] = file_record(b'isd_config.ini', 0x38d0, 699, 2)
    image[96:128] = file_record(b'app_dir_head', 0x4000, 0xffffffff, 0x81)
    image[128:160] = file_record(b'key_mac', 0xff000, 0x1000, 0x12)
    blob = bytes(range(16, 48))
    image[0x38d0:0x38f2] = blob + struct.pack('<H', crc16(blob))
    return bytes(image)


def check_round_trip(original, container):
    changed = install(original, container)
    before_config = enc(original[64:96])
    at = struct.unpack_from('<I', before_config, 4)[0]
    assert len(changed) == len(original)
    assert changed[:32] == original[:32]
    assert changed[96:0xa0] == original[96:0xa0]
    assert changed[0x4000:] == original[0x4000:]
    assert extract(changed) == container
    # Everything between the bootloader and the chip-key record is erased.
    assert changed[0xa0+len(container):0x3fdc] == bytes([255])*(0x3fdc-0xa0-len(container))
    assert changed[0x3fdc:0x3ffe] == original[at:at+34]
    assert changed[0x3ffe:0x4000] == b'\xff\xff'
    before_record, after_record = enc(original[32:64]), enc(changed[32:64])
    assert before_record[12:] == after_record[12:]
    assert struct.unpack_from('<H', after_record, 2)[0] == crc16(container)
    assert struct.unpack_from('<II', after_record, 4) == (0xa0, len(container))
    after_config = enc(changed[64:96])
    assert after_config[12:] == before_config[12:]
    assert crc16(after_config[2:]) == struct.unpack_from('<H', after_config)[0]
    assert struct.unpack_from('<H', after_config, 2)[0] == crc16(original[at:at+34])
    assert struct.unpack_from('<II', after_config, 4) == (0x3fdc, 34)
    assert install(changed, container) == changed


def check_adverse_layouts(container):
    original = synthetic_image()
    check_round_trip(original, container)
    # FM-1 016's layout: the configuration further on, and longer.
    moved = bytearray(original)
    moved[64:96] = file_record(b'isd_config.ini', 0x3938, 755, 2)
    moved[0x3938:0x395a] = original[0x38d0:0x38f2]
    check_round_trip(bytes(moved), container)
    # Only the chip-key record is kept: without a good one, nothing is written.
    damaged = bytearray(original)
    damaged[0x38d0] ^= 1
    refuses(install, damaged, container)
    # The full slot is rewritten, including padding after a smaller container.
    for slot, name, address, length, attribute in (
        (64, b'isd_config.ini', 0x2000, 16, 2),
        (64, b'isd_config.ini', 0x38c8, 16, 2),
        # A config before the slot does not overlap it, but cannot follow A.
        (64, b'isd_config.ini', 0x20, 16, 2),
        (96, b'app_dir_head', 0x2000, 0xffffffff, 0x81),
        (96, b'app_dir_head', 0x2000, 0, 0x81),
        (96, b'app_dir_head', 0x90, 0xffffffff, 0x81),
        # A finite directory length is an address of a bank-info record.
        (96, b'app_dir_head', 0x4000, 0x2000, 0x81),
        (128, b'key_mac', 0x3000, 0x1000, 0x12),
        (128, b'other_record', 0x1000, 16, 0x12),
        (128, b'other_record', 0x90, 0x20, 0x12),
        (128, b'other_record', 0xffff0, 0x20, 0x12),
        (32, b'uboot.boot', 0x2000, 0x1000, 0),
        (32, b'uboot.boot', 0xa0, 0x3f3d, 0),
        (64, b'isd_config.ini', 0x3fe0, 34, 2),
        (64, b'isd_config.ini', 0x38d0, 33, 2),
    ):
        bad = bytearray(original)
        bad[slot:slot+32] = file_record(name, address, length, attribute)
        if slot == 64 and address == 0x2000:
            bad[address:address+16] = b'CONFIG-SENTINEL!'
        snapshot = bytes(bad)
        refuses(install, bad, container)
        assert bytes(bad) == snapshot
    for slot in (64, 96, 128):
        bad = bytearray(original)
        bad[slot] ^= 1
        refuses(install, bad, container)


# §4.4: the packer's filter and stage 0's undo agree where a call's target looks like another call.
crafted = bytearray()
for n in range(600):
    crafted += bytes([0xbf, 0xea]) * (n % 4 + 1) + struct.pack('<H', n*37 & 0xffff) + bytes([n & 255, 0x20, 0, 0])
with tempfile.TemporaryDirectory() as directory:
    raw, packed = Path(directory, 'stage1.bin'), Path(directory, 'stage1.pack')
    raw.write_bytes(bytes(crafted))
    packed.write_bytes(pack_stage1(bytes(crafted), os.environ.get('UPKR', 'upkr')))
    subprocess.run(['build/upkr-test', str(packed), str(raw)], check=True, capture_output=True)
assert call_filter(bytes(crafted)) != bytes(crafted)

fixture_images = {}
for name, (sha, checksum) in FIXTURES.items():
    try:
        original = Path('testdata', name).read_bytes()
    except FileNotFoundError:
        print(f'SKIP: {name} not present (container install round trips)')
        continue
    assert hashlib.sha256(original).hexdigest() == sha
    assert zlib.crc32(original) == checksum
    assert original[0xa0:0x38d0] == bytes([255]) * 0x3830
    fixture_images[name] = original


assert crc16(b'123456789') == 0x31c3
for board in ('fm1', 'smk37'):
    directory = Path('build', board)
    payload = (directory / 'payload.bin').read_bytes()
    own_container = (directory / 'uboot.boot').read_bytes()
    assert payload[0x50:0x70] == boot_version().encode('ascii').ljust(32, b'\0')
    assert own_container == pack(payload, board)
    assert validate(own_container, board) == board
    assert len(own_container) <= 0x3f3c
    inspect(directory / 'stage0.elf', directory / 'stage0.bin', 0x1c02000, 0x1c0a800, 'boot_entry', True)
    # §4.4: stage 1 starts where stage 0 ends, and the hash ignores only the version.
    stage0, stage1 = (directory / 'stage0.bin').read_bytes(), (directory / 'stage1.bin').read_bytes()
    packed = (directory / 'stage1.pack').read_bytes()
    assert payload == stage0+packed
    assert len(packed) < len(stage1)
    joined = Path('build', f'{board}-joined.bin')
    sizes = join(directory / 'stage0.elf', directory / 'stage0.bin', directory / 'stage1.elf', directory / 'stage1.bin', directory / 'stage1.pack', joined)
    assert joined.read_bytes() == payload
    joined.unlink()
    assert sizes['stage0_bytes'] == len(stage0) and sizes['stage1_bytes'] == len(stage1)
    assert sizes['stage0_code_sha256'] == stage0_code_hash(stage0)
    assert stage0_code_hash(stage0[:0x50]+bytes([255])*32+stage0[0x70:]) == sizes['stage0_code_sha256']
    assert stage0_code_hash(stage0[:0x70]+bytes([stage0[0x70]^1])+stage0[0x71:]) != sizes['stage0_code_sha256']
    assert stage0_code_hash(bytes([stage0[0]^1])+stage0[1:]) != sizes['stage0_code_sha256']
    refuses(join, directory / 'stage0.elf', directory / 'stage0.bin', directory / 'stage0.elf', directory / 'stage0.bin', directory / 'stage1.pack', joined)
    refuses(validate, own_container, 'smk37' if board == 'fm1' else 'fm1')
    refuses(pack, payload + bytes(0x3f2d), board)
    for at in (0, 4, 12, 14, 16, 0x50, len(own_container)-1):
        bad = bytearray(own_container)
        bad[at] ^= 1
        refuses(validate, bad)
    check_adverse_layouts(own_container)
    for name, original in fixture_images.items():
        check_round_trip(original, own_container)
        # Install is repeatable and never mutates the input fixture on disk.
        assert Path('testdata', name).read_bytes() == original
for slot, base, limit in (('a', 0x1c20000, 0x1c28000), ('b', 0x1c28000, 0x1c30000)):
    binary = Path('build/loader', f'mflr-loader-{slot}.bin')
    inspect(Path(f'build/loader-{slot}/loader.elf'), binary, base, limit, 'loader_entry')
    raw = binary.read_bytes()
    marker = raw.find(b'MFLR')
    assert marker >= 0 and raw[marker:marker+7] == b'MFLR1.3'
print('PASS: containers, ELF/RAM limits, loader version markers, synthetic install safety '
      f'and {2 * len(fixture_images)} private-fixture install round trips')
