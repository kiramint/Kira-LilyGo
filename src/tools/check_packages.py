"""Check actual build artifacts with the same Xtensa ELF validator as System."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def verify(build):
    project = json.loads((build / 'project_description.json').read_text())
    host = (build / project['app_elf']).read_bytes()
    firmware = (build / project['app_bin']).read_bytes()
    digest = hashlib.sha256(host).digest()
    # ESP image header (24), first segment header (8), esp_app_desc SHA offset (144).
    assert firmware[176:208] == digest, 'flashed image and host ELF differ'
    loader = Path(project['project_path']) / 'managed_components/espressif__elf_loader'
    with tempfile.TemporaryDirectory() as tmp:
        library = Path(tmp) / 'validator.so'
        subprocess.run([os.getenv('HOST_CC', 'cc'), '-std=c11', '-shared', '-fPIC',
                        '-DCONFIG_IDF_TARGET_ARCH_XTENSA=1', '-DCONFIG_ELF_LOADER_BUS_ADDRESS_MIRROR=1',
                        '-I', str(loader / 'include'), '-I', str(loader / 'tests/host/include'),
                        str(loader / 'src/esp_elf_validate.c'), '-o', str(library)], check=True)
        validate = ctypes.CDLL(str(library)).esp_elf_validate
        validate.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        manifests = sorted(build.glob('sdcard/apps/*/manifest.json'))
        assert manifests, 'no app packages built'
        nm = project['c_compiler'].removesuffix('gcc') + 'nm'
        symbols = subprocess.check_output([nm, '-g', '--defined-only', str(build / project['app_elf'])], text=True)
        addresses = {row[2]: int(row[0], 16) for line in symbols.splitlines() if len(row := line.split()) == 3}
        imports = set()
        for path in manifests:
            data = json.loads(path.read_text())
            assert data['id'] == path.parent.name and data['target'] == 'esp32s3'
            assert data['firmware'] == digest.hex(), f'stale manifest {path}'
            undefined = subprocess.check_output([nm, '-D', '-u', str(path.parent / 'app.elf')], text=True)
            imports.update(line.split()[-1] for line in undefined.splitlines())
            image = (path.parent / 'app.elf').read_bytes()
            assert validate(image, len(image)) == 0, f'loader rejected {path.parent}'
            assert validate(image, len(image) // 2) != 0, 'accepted truncated ELF'
            print(f"{data['id']}: validated {len(image)} bytes, firmware {digest.hex()[:12]}")

        entries = [f'{{{json.dumps(name)}, 0x{addresses[name]:x}}},' for name in sorted(imports)]
        (Path(tmp) / 'host_imports.h').write_text(
            'static const struct {const char *name; uintptr_t address;} imports[] = {\n'
            + '\n'.join(entries) + '\n{NULL, 0}};\n')
        runner = Path(tmp) / 'check_loader'
        subprocess.run([os.getenv('HOST_CC', 'cc'), '-std=c11', '-D_GNU_SOURCE',
                        '-DCONFIG_IDF_TARGET_ARCH_XTENSA=1', '-DCONFIG_ELF_LOADER_BUS_ADDRESS_MIRROR=1',
                        '-ffunction-sections', '-fdata-sections', '-Wno-pointer-to-int-cast',
                        '-Wno-int-to-pointer-cast', '-Wno-format',
                        '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections',
                        '-I', tmp, '-I', str(loader / 'include'), '-I', str(loader / 'tests/host/include'),
                        str(Path(__file__).with_name('check_loader.c')), str(loader / 'src/esp_elf.c'),
                        str(loader / 'src/esp_elf_validate.c'), str(loader / 'src/arch/esp_elf_xtensa.c'),
                        '-o', str(runner)], check=True)
        subprocess.run([str(runner), *(str(m.parent / 'app.elf') for m in manifests)], check=True)
        print('100 concurrent-load/unload cycles, missing imports and allocation failure passed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path, nargs='?', default=Path(__file__).resolve().parents[1] / 'build')
    verify(parser.parse_args().build)
