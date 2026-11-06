# SPDX-License-Identifier: MIT
"""Add the trailer to a flat binary, making an SD card stage 2 image (docs/SPEC.md §11.5)."""
import argparse
from pathlib import Path
import struct
from bankcb import crc16

MIN, MAX = 16, 0x8000
TRAILER = 8


def wrap(code):
    if not MIN <= len(code)+TRAILER <= MAX:
        raise ValueError(f'{len(code)+TRAILER} bytes with the trailer, not {MIN} to {MAX}')
    return code+b'STG2'+struct.pack('<H', crc16(code))+bytes(2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path, help='the flat binary, linked for 0x01C28000')
    parser.add_argument('output', type=Path, help='the image, MENU.BIN on the card')
    args = parser.parse_args()
    try:
        image = wrap(args.input.read_bytes())
    except (OSError, ValueError) as e:
        parser.exit(1, f'{args.input}: {e}\n')
    args.output.write_bytes(image)


if __name__ == '__main__':
    main()
