# SPDX-License-Identifier: MIT
"""Exercise clean/dirty revisions, ignored files and exported source builds."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from version import boot_version, write_header


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()


(ROOT / 'build').mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix='version-test-', dir=ROOT / 'build') as temp:
    root = Path(temp)
    base = (ROOT / 'VERSION').read_text().strip()
    (root / 'VERSION').write_text(base + '\n')
    # No checkout, including an export nested inside another repository.
    assert boot_version(root) == base
    git(root, 'init', '-q')
    git(root, 'config', 'user.name', 'Version test')
    git(root, 'config', 'user.email', 'version-test@example.invalid')
    (root / '.gitignore').write_text('/build/\n/toolchain/\n/testdata/\n')
    git(root, 'add', 'VERSION', '.gitignore')
    git(root, 'commit', '-qm', 'Synthetic version baseline')
    revision = git(root, 'rev-parse', 'HEAD')[:7]
    clean = base + '-' + revision
    assert boot_version(root) == clean
    assert len(clean + '-dirty') <= 20

    header = root / 'build' / 'boot_version.h'
    write_header(header, root)
    unchanged = header.stat().st_mtime_ns
    write_header(header, root)
    assert header.stat().st_mtime_ns == unchanged
    assert f'"{clean}"' in header.read_text()

    for ignored in ('build', 'toolchain', 'testdata'):
        (root / ignored).mkdir(exist_ok=True)
        (root / ignored / 'untracked').write_bytes(b'synthetic')
    (root / 'untracked').write_text('untracked files are not tracked changes')
    assert boot_version(root) == clean
    exported = root / 'build' / 'export'
    exported.mkdir()
    (exported / 'VERSION').write_text(base + '\n')
    assert boot_version(exported) == base

    # Both unstaged and staged tracked differences add the suffix.
    (root / 'VERSION').write_text(base + '\n\n')
    assert boot_version(root) == clean + '-dirty'
    write_header(header, root)
    assert f'"{clean}-dirty"' in header.read_text()
    git(root, 'add', 'VERSION')
    assert boot_version(root) == clean + '-dirty'
    git(root, 'commit', '-qm', 'Synthetic source revision')
    next_clean = base + '-' + git(root, 'rev-parse', 'HEAD')[:7]
    assert next_clean != clean
    assert boot_version(root) == next_clean
    write_header(header, root)
    assert f'"{next_clean}"' in header.read_text()
    # A tracked deletion (other than the required version file) is dirty too.
    (root / '.gitignore').unlink()
    assert boot_version(root) == next_clean + '-dirty'

print('PASS: boot versions follow clean/staged/unstaged revisions, ignore untracked files, '
      'fit the full row, and remain stable outside git')
