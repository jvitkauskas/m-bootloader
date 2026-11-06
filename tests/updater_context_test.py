# SPDX-License-Identifier: MIT
"""§6.4: M-VAVE's updater reads what the bootloader hands it in words, so these must stay aligned.
Nothing else shows this: a build that gets it wrong runs well until an official update hangs."""
import subprocess
import sys

NAMES = {'clock_meta', 'memory_meta', 'update_meta', 'request'}
nm, *elves = sys.argv[1:]
for elf in elves:
    found = {}
    for line in subprocess.run([nm, elf], check=True, capture_output=True, text=True).stdout.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2] in NAMES:
            found[fields[2]] = int(fields[0], 16)
    assert set(found) == NAMES, f'{elf}: found only {sorted(found)}'
    for name, address in found.items():
        assert address%4 == 0, f'{elf}: {name} at {address:#x} is not word aligned'
print('PASS: the updater context is word aligned on', len(elves), 'boards')
