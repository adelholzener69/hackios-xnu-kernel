#!/usr/bin/env python3
"""Will the HakiOS raphael UEFI patches work on YOUR phone's firmware?

The UEFI is built from drivers extracted out of your phone's own XBL, with three small byte
patches. Each patch is guarded by the SHA-256 of that module's original bytes. This tool
extracts the modules from your xbl image and tells you whether those three still match.

  devices/raphael/kit/check-xbl.py [out/raphael/firmware/xbl.img [patches.json]]

Needs `pip install uefi_firmware`. Only reads; writes nothing.
Exit status: 0 = all patched modules match, 1 = at least one differs (do not build), 2 = bad input.
"""
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

try:
    from uefi_firmware.uefi import FirmwareVolume, FirmwareFile, FirmwareFileSystemSection
except ImportError:
    sys.exit('missing dependency: pip install uefi_firmware')

ROOT = Path(__file__).resolve().parents[3]
if len(sys.argv) > 3:
    sys.exit(__doc__)
xbl = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'out/raphael/firmware/xbl.img'
patches_json = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / 'devices/raphael/uefi/raphael-firmware-patches.json'
raw = xbl.read_bytes()
if b'_FVH' not in raw:
    print('not an XBL image (no firmware volume found)')
    sys.exit(2)
print('xbl sha256 :', hashlib.sha256(raw).hexdigest())
print('xbl size   :', len(raw), 'bytes')

fv = FirmwareVolume(raw[raw.index(b'_FVH') - 40:])
if not (fv.valid_header and fv.process()):
    print('could not parse the firmware volume')
    sys.exit(2)


def walk(obj):
    yield obj
    for child in obj.objects:
        if child is not None:
            yield from walk(child)


def sections(obj):
    for child in obj.objects:
        if child is None or isinstance(child, FirmwareFile):
            continue
        if isinstance(child, FirmwareFileSystemSection):
            yield child
        yield from sections(child)


modules = {}
for f in (o for o in walk(fv) if isinstance(o, FirmwareFile)):
    exe = next((s for s in sections(f) if s.type in (0x10, 0x12)), None)
    if exe is None:
        continue
    code = exe.data
    paths = [p.decode().replace('\\', '/') for p in re.findall(rb'[a-zA-Z/\\0-9_\-\.]*\.dll\b', code)]
    pdb = next((p.split('/AARCH64/')[-1] for p in paths if '/AARCH64/' in p), None)
    if pdb:
        parts = pdb.split('/')
        modules[parts[-3]] = hashlib.sha256(code).hexdigest()
print('modules    :', len(modules), 'extracted')

bad = 0
for patch in json.loads(patches_json.read_text()):
    name = patch['module'].split('/')[-1]
    got = modules.get(name)
    ok = got == patch['original_sha256']
    bad += not ok
    print(f"  {'MATCH    ' if ok else 'DIFFERENT'}  {patch['module']}  expected {patch['original_sha256'][:12]}  yours {(got or 'missing')[:12]}")
print('VERDICT    :', 'compatible, the patches will apply' if not bad else 'NOT compatible: these patches need re-validating for your firmware')
sys.exit(1 if bad else 0)
