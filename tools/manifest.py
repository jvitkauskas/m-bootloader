# SPDX-License-Identifier: MIT
"""Hash only the independently built release artifacts; never include fixtures."""
import hashlib
import json
from pathlib import Path

PRODUCTS = {
    'build/fm1/uboot.boot': 'build/fm1/size.json',
    'build/smk37/uboot.boot': 'build/smk37/size.json',
    'build/loader/mflr-loader-a.bin': 'build/loader-a/size.json',
    'build/loader/mflr-loader-b.bin': 'build/loader-b/size.json',
    'build/fm1/uboot.fwsc': 'build/fm1/installer-size.json',
    'build/smk37/uboot-pro.fwsc': 'build/smk37/installer-size.json',
    'build/smk37/uboot-elite.fwsc': 'build/smk37/installer-size.json',
    'build/fm1/stage-ram.bin': 'build/fm1/stage-ram-size.json',
    'build/smk37/stage-ram.bin': 'build/smk37/stage-ram-size.json',
    'build/fm1/stage.fwsc': 'build/fm1/stage-ota-size.json',
    'build/smk37/stage-pro.fwsc': 'build/smk37/stage-ota-size.json',
    'build/smk37/stage-elite.fwsc': 'build/smk37/stage-ota-size.json',
    'build/fm1/stage-listed.fwsc': 'build/fm1/stage-ota-size.json',
    'build/smk37/stage-pro-listed.fwsc': 'build/smk37/stage-ota-size.json',
    'build/smk37/stage-elite-listed.fwsc': 'build/smk37/stage-ota-size.json',
}


def main():
    result = {}
    for name, size_path in PRODUCTS.items():
        data = Path(name).read_bytes()
        result[name] = {**json.loads(Path(size_path).read_text()), 'file_bytes': len(data),
                        'sha256': hashlib.sha256(data).hexdigest()}
    print(json.dumps(result, sort_keys=True, indent=2))


if __name__ == '__main__':
    main()
