# SPDX-License-Identifier: MIT
"""Verify the stage 2 and app trailers against a vector taken from core_crc16, and the size limits."""
from pathlib import Path
import subprocess
import sys
import tempfile
TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
from stage2 import APP_MAX, MAX, MIN, TRAILER, wrap, wrap_app


def refused(size, fn=wrap):
    try:
        fn(bytes(size))
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

assert wrap_app(code, 'any') == code+b'APP1\xdf\xa6\0\x01'
assert wrap_app(code, 'fm1') == code+b'APP1\xdf\xa6\x01\x01'
assert wrap_app(code, 'smk37') == code+b'APP1\xdf\xa6\x02\x01'
app = lambda data: wrap_app(data, 'any')
assert not refused(MIN-TRAILER, app)
assert refused(MIN-TRAILER-1, app)
assert not refused(APP_MAX-TRAILER, app)
assert refused(APP_MAX-TRAILER+1, app)

with tempfile.TemporaryDirectory() as folder:
    source, image = Path(folder)/'in.bin', Path(folder)/'MENU.BIN'
    source.write_bytes(code)
    subprocess.run([sys.executable, TOOLS/'stage2.py', source, image], check=True)
    assert image.read_bytes() == wrap(code)
    subprocess.run([sys.executable, TOOLS/'stage2.py', '--app', 'smk37', source, image], check=True)
    assert image.read_bytes() == wrap_app(code, 'smk37')
    source.write_bytes(bytes(MAX))
    image.unlink()
    failed = subprocess.run([sys.executable, TOOLS/'stage2.py', source, image], capture_output=True)
    assert failed.returncode == 1 and b'with the trailer' in failed.stderr
    assert not image.exists()
print('stage 2 and app trailers OK')
