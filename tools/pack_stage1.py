# SPDX-License-Identifier: MIT
"""Pack stage 1 for stage 0 to unpack: a call filter, then Upkr with two parity contexts."""
import argparse
import os
from pathlib import Path
import subprocess
import struct
import tempfile
from bankcb import crc16

HEADER = 8


def call_filter(data):
    """Make relative calls absolute, so repeated calls look alike. Undone in src/upkr.c."""
    out, at = bytearray(data), 0
    while at+4 <= len(out):
        if out[at] == 0xbf and out[at+1] == 0xea:
            struct.pack_into('<H', out, at+2, (struct.unpack_from('<H', out, at+2)[0]+(at >> 1))&0xffff)
            at += 4
        else:
            at += 2
    return bytes(out)


def pack(data, upkr):
    with tempfile.TemporaryDirectory() as directory:
        source, target = Path(directory, 'in'), Path(directory, 'out')
        source.write_bytes(call_filter(data))
        subprocess.run([upkr, '-9', '-p', '2', str(source), str(target)], check=True, capture_output=True)
        packed = target.read_bytes()
    return struct.pack('<HHHH', len(packed), len(data), crc16(data), 0)+packed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage1')
    parser.add_argument('out')
    args = parser.parse_args()
    Path(args.out).write_bytes(pack(Path(args.stage1).read_bytes(), os.environ.get('UPKR', 'upkr')))


if __name__ == '__main__':
    main()
