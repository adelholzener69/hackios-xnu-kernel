#!/usr/bin/env bash
# Build the boot image (UEFI + XNU loader) for YOUR phone's firmware.
#
#   devices/raphael/kit/build-uefi-image.sh [path/to/this/repo]
#
# Needs first: devices/raphael/kit/pull-firmware.sh (your xbl.img) and
# devices/raphael/kit/build-raphael-dtb.py (out/raphael/raphael.dtb). Result: out/raphael/boot.img, which you
# start with `fastboot boot` (RAM only, nothing is flashed).
#
# It creates a Python virtualenv inside the checkout (out/mu-venv) and installs the packages
# Mu-Silicium's own pip-requirements.txt lists, plus uefi_firmware and lzfse. Nothing system-wide.
set -euo pipefail

KIT=$(cd "$(dirname "$0")" && pwd)
HAKIOS=${1:-$(cd "$KIT/../../.." && pwd)}
cd "$HAKIOS"
ROOT=$PWD

[ -f out/raphael/firmware/xbl.img ] || { echo "missing out/raphael/firmware/xbl.img: run devices/raphael/kit/pull-firmware.sh" >&2; exit 2; }
[ -f out/raphael/raphael.dtb ] || { echo "missing out/raphael/raphael.dtb: run devices/raphael/kit/build-raphael-dtb.py" >&2; exit 2; }

echo "== Mu-Silicium sources (about 5 GB, once) =="
[ -d Mu-Silicium ] || devices/raphael/uefi/apply.sh

echo "== Python environment =="
[ -x out/mu-venv/bin/python ] || python3 -m venv out/mu-venv
out/mu-venv/bin/pip install -q -r Mu-Silicium/pip-requirements.txt uefi_firmware lzfse

echo "== is your firmware compatible with the patched UEFI modules? =="
out/mu-venv/bin/python "$KIT/check-xbl.py" out/raphael/firmware/xbl.img \
	devices/raphael/uefi/raphael-firmware-patches.json

echo "== extract the XBL modules from your phone =="
rm -rf Mu-Silicium/Binaries/raphael            # the extractor patches in place, so start clean
out/mu-venv/bin/python tools/extract-raphael-uefi.py
mkdir -p Mu-Silicium/Resources/DTBs
cp out/raphael/raphael.dtb Mu-Silicium/Resources/DTBs/raphael.dtb

echo "== build the UEFI (DEBUG: it shows its log on screen) =="
# EDK II's host tools are old C that GCC 15+ rejects by default: build them as gnu17.
mkdir -p out/legacycc
for c in cc gcc; do
	printf '#!/bin/sh\nexec /usr/bin/%s -std=gnu17 -Wno-error "$@"\n' "$c" > "out/legacycc/$c"
	chmod +x "out/legacycc/$c"
done
(
	cd Mu-Silicium
	export PATH="$ROOT/out/legacycc:$ROOT/out/mu-venv/bin:$PATH" CLANGPDB_BIN=/usr/bin/ CLANG_BIN=/usr/bin/ CLANGPDB_AARCH64_PREFIX=llvm-
	python build_uefi.py -d raphael -r DEBUG
)

echo "== build the XNU loader =="
export PATH="$ROOT/out/mu-venv/bin:$ROOT/xnu-deps/host/bin:$PATH"
xnuloader/mkidentity.sh           # the iPhone identity, from Apple's public IPSW (one 34 KB range request)
xnuloader/build.sh

echo "== pack the boot image =="
ROOT=$PWD source devices/raphael/device.conf
export UEFI_TARGET DTB FD_BASE FD_SIZE PAYLOAD_BASE
python3 xnuloader/mkpayload.py -o out/raphael/usb-payload.bin loader.efi=xnuloader/loader.efi --usb
python3 mkxnuimg.py -p out/raphael/usb-payload.bin -o out/raphael/boot.img --mode DEBUG
ls -l out/raphael/boot.img
echo "done: fastboot boot out/raphael/boot.img   (then see README, 'Run it')"
