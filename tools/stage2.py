# SPDX-License-Identifier: MIT
"""Add the trailer to a flat binary, making an SD card stage 2 image (docs/SPEC.md §11.5) or a RAM app (§11.8)."""
import argparse
from pathlib import Path
import struct
from bankcb import crc16

MIN, MAX = 16, 0x8000
APP_MAX = 0x4fd80
TRAILER = 8
APP_BOARDS = {'any': 0, 'fm1': 1, 'smk37': 2}
APP_FORMAT = 1


def check_size(code, limit):
    if not MIN <= len(code)+TRAILER <= limit:
        raise ValueError(f'{len(code)+TRAILER} bytes with the trailer, not {MIN} to {limit}')


def wrap(code):
    check_size(code, MAX)
    return code+b'STG2'+struct.pack('<H', crc16(code))+bytes(2)


def wrap_app(code, board):
    check_size(code, APP_MAX)
    return code+b'APP1'+struct.pack('<HBB', crc16(code), APP_BOARDS[board], APP_FORMAT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', choices=APP_BOARDS, metavar='BOARD',
                        help='make a RAM app for this board: any, fm1 or smk37')
    parser.add_argument('input', type=Path, help='the flat binary, linked for 0x01C28000, or 0x01C30000 with --app')
    parser.add_argument('output', type=Path, help='the image: MENU.BIN on the card, or the app')
    args = parser.parse_args()
    try:
        code = args.input.read_bytes()
        image = wrap_app(code, args.app) if args.app else wrap(code)
    except (OSError, ValueError) as e:
        parser.exit(1, f'{args.input}: {e}\n')
    args.output.write_bytes(image)


if __name__ == '__main__':
    main()
