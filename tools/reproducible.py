# SPDX-License-Identifier: MIT
"""Repeat all compilation/linking and require identical release bytes: of the artifacts named (the build's own, as
make passes them), or of every release product."""
import hashlib
from pathlib import Path
import subprocess
import sys
from manifest import PRODUCTS

ARTIFACTS = sys.argv[1:] or list(PRODUCTS)


def hashes():
    return {name: hashlib.sha256(Path(name).read_bytes()).hexdigest() for name in ARTIFACTS}


before = hashes()
subprocess.run(['make', '-B', '-j2', 'all'], check=True)
after = hashes()
if before != after:
    raise SystemExit('FAIL: repeated build changed artifact bytes')
print(f'PASS: all {len(ARTIFACTS)} artifacts are byte-identical after a forced rebuild')
