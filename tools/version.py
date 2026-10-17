# SPDX-License-Identifier: MIT
"""Generate a bootloader-only version from VERSION and tracked git state."""
import argparse
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def boot_version(root=ROOT):
    root = Path(root).resolve()
    version = (root / 'VERSION').read_text().strip()
    if not re.fullmatch(r'[0-9]+\.[0-9]+', version):
        raise ValueError('VERSION must contain major.minor')

    def git(*args):
        return subprocess.run(['git', '-C', str(root), *args], check=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True).stdout.strip()

    try:
        checkout = Path(git('rev-parse', '--show-toplevel')).resolve()
    except (FileNotFoundError, subprocess.CalledProcessError):
        checkout = None
    # An exported source tree may itself be inside an unrelated checkout.
    if checkout == root:
        revision = git('rev-parse', '--verify', 'HEAD')[:7]
        dirty = subprocess.run(['git', '-C', str(root), 'diff', '--quiet', 'HEAD', '--'],
                               check=False).returncode
        if dirty not in (0, 1):
            raise RuntimeError('could not determine tracked git state')
        version += '-' + revision + ('-dirty' if dirty else '')
    if len(version) > 20:
        raise ValueError('bootloader version exceeds the 20-column menu row')
    return version


def write_header(destination, root=ROOT):
    destination = Path(destination)
    content = ('/* SPDX-License-Identifier: MIT */\n'
               '#ifndef BOOT_VERSION_H\n#define BOOT_VERSION_H\n'
               f'#define BOOT_VERSION {json.dumps(boot_version(root))}\n'
               '#endif\n')
    if not destination.exists() or destination.read_text() != content:
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(content)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('header', type=Path)
    write_header(parser.parse_args().header)
