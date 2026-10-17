# SPDX-License-Identifier: MIT
"""Repeat all compilation/linking and require identical release bytes."""
import hashlib
from pathlib import Path
import subprocess
from manifest import PRODUCTS


def hashes():
    return {name: hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in PRODUCTS}


before = hashes()
subprocess.run(['make', '-B', '-j2', 'all'], check=True)
after = hashes()
if before != after:
    raise SystemExit('FAIL: repeated build changed release artifact bytes')
print('PASS: all four release artifacts are byte-identical after a forced rebuild')
