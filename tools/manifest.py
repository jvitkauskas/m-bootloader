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
