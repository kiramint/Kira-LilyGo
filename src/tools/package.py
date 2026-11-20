"""Generate the firmware import table and bind SD packages to the exact host ELF."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


def exports(nm, output, images):
    names = set()
    for image in images:
        symbols = subprocess.check_output([nm, '-D', '-u', image], text=True)
        for line in symbols.splitlines():
            kind, name = line.split()
            if kind != 'U' or not re.fullmatch(r'[a-zA-Z_$][\w.$]*', name):
                raise ValueError(f'unsupported import in {image}: {line}')
            names.add(name)
    # Opaque address aliases handle both data and mangled C++ functions.
    # A missing host definition therefore fails the ordinary firmware link.
    definitions = ['#include "esp_elf.h"']
    entries = []
    for i, name in enumerate(sorted(names)):
        definitions.append(f'extern char import_{i} __asm__("{name}");')
        entries.append(f'    {{"{name}", &import_{i}}},')
    Path(output).write_text('\n'.join(definitions + ['const struct esp_elfsym app_exports[] = {']
                                     + entries + ['    ESP_ELFSYM_END', '};', '']))


def manifest(metadata, firmware, output):
    data = json.loads(Path(metadata).read_text())
    if data['id'] != Path(output).parent.name:
        raise ValueError('manifest id must match package directory')
    data.update(target='esp32s3', firmware=hashlib.sha256(Path(firmware).read_bytes()).hexdigest())
    Path(output).write_text(json.dumps(data, indent=2) + '\n')


if __name__ == '__main__':
    if sys.argv[1] == 'exports':
        exports(sys.argv[2], sys.argv[3], sys.argv[4:])
    else:
        manifest(*sys.argv[2:])
