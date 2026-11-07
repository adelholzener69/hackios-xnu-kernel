#!/usr/bin/env python3
"""Build raphael.dtb the way the stock bootloader does: SM8150 base tree + the raphael board overlay.

The upstream HakiOS build wants this file but builds it from a private kernel tree. Both halves
are already on your phone's own firmware, so this script takes them from files you have:

  --boot-image  an Android boot image whose kernel has the SM8150 v2 device tree appended
                (the official TWRP image for raphael works; so does the stock boot.img)
  --dtbo        the stock dtbo.img (from your ROM, or `dd` of the `dtbo` partition)

It finds the base tree (qcom,msm-id <0x153 ...>), finds the overlay whose model is "RAPHAEL"
with qcom,board-id <0x28 0x00>, merges them with fdtoverlay and checks the result.

Needs `dtc` and `fdtoverlay` (package `dtc`). Writes nothing outside --output.

  build-raphael-dtb.py --boot-image twrp.img --dtbo dtbo.img -o out/raphael/raphael.dtb
"""
import argparse
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('--boot-image', required=True, type=Path)
ap.add_argument('--dtbo', required=True, type=Path)
ap.add_argument('-o', '--output', required=True, type=Path)
args = ap.parse_args()


def dts_of(blob: bytes, work: Path) -> str:
    f = work / 'x.dtb'
    f.write_bytes(blob)
    return subprocess.run(['dtc', '-q', '-I', 'dtb', '-O', 'dts', str(f)], capture_output=True, text=True).stdout


def appended_fdts(kernel: bytes):
    """Every plausible FDT appended inside the kernel blob."""
    i = 0
    while True:
        i = kernel.find(b'\xd0\x0d\xfe\xed', i)
        if i < 0:
            return
        total, version = struct.unpack_from('>I', kernel, i + 4)[0], struct.unpack_from('>I', kernel, i + 20)[0]
        if 0x100 < total < 0x400000 and i + total <= len(kernel) and version in (16, 17):
            yield kernel[i:i + total]
        i += 4


boot = args.boot_image.read_bytes()
if boot[:8] != b'ANDROID!':
    sys.exit('not an Android boot image: ' + str(args.boot_image))
kernel_size, page = struct.unpack_from('<I', boot, 8)[0], struct.unpack_from('<I', boot, 36)[0]
kernel = boot[page:page + kernel_size]

with tempfile.TemporaryDirectory(prefix='raphael-dtb-') as tmp:
    work = Path(tmp)

    base = None
    for blob in appended_fdts(kernel):
        text = dts_of(blob, work)
        if re.search(r'qcom,msm-id = <0x153 0x20000>', text) and 'SM8150 v2' in text:
            base = blob
            break
    if base is None:
        sys.exit('no SM8150 v2 base tree (msm-id 0x153/0x20000) appended to that boot image kernel')

    d = args.dtbo.read_bytes()
    magic, _total, _hsz, esz, count, eoff = struct.unpack_from('>6I', d, 0)
    if magic != 0xd7b7ab1e:
        sys.exit('not an Android dtbo image: ' + str(args.dtbo))
    overlay = None
    for n in range(count):
        size, off = struct.unpack_from('>2I', d, eoff + n * esz)
        text = dts_of(d[off:off + size], work)
        if 'model = "RAPHAEL";' in text and 'qcom,board-id = <0x28 0x00>;' in text:
            overlay = text
            break
    if overlay is None:
        sys.exit('no RAPHAEL overlay (board-id 0x28) in that dtbo image: is it from a raphael?')

    # Android overlays keep model/board-id on the root node; libfdt's merger only merges fragments,
    # so turn the root node into a fragment targeting "/".
    fixed = re.sub(r'(?m)^(\s*)/\s*\{', r'\1&{/} {', overlay)
    (work / 'base.dtb').write_bytes(base)
    (work / 'overlay.dts').write_text(fixed)
    subprocess.run(['dtc', '-q', '-@', '-I', 'dts', '-O', 'dtb', '-b', '0', '-o', str(work / 'overlay.dtbo'),
                    str(work / 'overlay.dts')], check=True, stderr=subprocess.DEVNULL)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['fdtoverlay', '-i', str(work / 'base.dtb'), '-o', str(args.output), str(work / 'overlay.dtbo')],
                   check=True)

merged = subprocess.run(['dtc', '-q', '-I', 'dtb', '-O', 'dts', str(args.output)], capture_output=True, text=True).stdout
for needle in ('model = "RAPHAEL";', 'qcom,board-id = <0x28 0x00>;', 'qcom,msm-id = <0x153 0x20000>;'):
    if needle not in merged:
        sys.exit('merged tree is missing ' + needle)
print('wrote', args.output, args.output.stat().st_size, 'bytes: model RAPHAEL, board-id 0x28, msm-id 0x153')
