# SPDX-License-Identifier: MIT
"""Verify the stage 2 trailer against a vector taken from core_crc16, and the size limits."""
from pathlib import Path
import subprocess
import sys
import tempfile
TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
from stage2 import MAX, MIN, TRAILER, wrap


def refused(size):
    try:
        wrap(bytes(size))
    except ValueError:
        return True
    return False


# 16 bytes of (i*7+3), as tests/sdboot_test.c builds its images; the CRC is what src/core.c gives for them.
code = bytes((i*7+3) & 255 for i in range(16))
assert wrap(code) == code+b'STG2\xdf\xa6\0\0'

assert not refused(MIN-TRAILER)
assert refused(MIN-TRAILER-1)
assert not refused(MAX-TRAILER)
assert refused(MAX-TRAILER+1)

with tempfile.TemporaryDirectory() as folder:
    source, image = Path(folder)/'in.bin', Path(folder)/'MENU.BIN'
    source.write_bytes(code)
    subprocess.run([sys.executable, TOOLS/'stage2.py', source, image], check=True)
    assert image.read_bytes() == wrap(code)
    source.write_bytes(bytes(MAX))
    image.unlink()
    failed = subprocess.run([sys.executable, TOOLS/'stage2.py', source, image], capture_output=True)
    assert failed.returncode == 1 and b'with the trailer' in failed.stderr
    assert not image.exists()
print('stage 2 trailer OK')
