#!/usr/bin/env bash
# Copy the files HakiOS needs from YOUR phone, read-only, into the HakiOS checkout.
#
#   devices/raphael/kit/pull-firmware.sh [path/to/this/repo]     (default: the repository this script is in)
#
# Run it with the phone in a recovery that gives `adb` root (TWRP). It only reads: the modem
# partition is mounted read-only and the other partitions are read with `cat`.
#
# Produces (all private to your phone, owner-only permissions, NEVER publish them):
#   <repo>/fw-raphael/        modem firmware for Wi-Fi + the four NV partitions + two helper files
#   <repo>/out/raphael/firmware/xbl.img, xbl_config.img   used to build the UEFI for your firmware
set -euo pipefail

HAKIOS=${1:-$(cd "$(dirname "$0")/../../.." && pwd)}
[ -d "$HAKIOS" ] || { echo "no such directory: $HAKIOS" >&2; exit 2; }
cd "$HAKIOS"
umask 077

command -v adb >/dev/null || { echo "adb not found" >&2; exit 2; }
adb get-state >/dev/null 2>&1 || { echo "no adb device: boot TWRP and connect the phone" >&2; exit 2; }

device=$(adb shell getprop ro.product.device | tr -d '\r')
[ "$device" = raphael ] || { echo "this phone says '$device', not raphael: stopping" >&2; exit 2; }
adb shell id | tr -d '\r' | grep -q 'uid=0' || { echo "adb is not root: use TWRP" >&2; exit 2; }

# Board data file: global phones use bd_f11gl.bin, Chinese ones bd_f11cn.bin.
hwc=$(adb shell getprop ro.boot.hwc | tr -d '\r')
case "$hwc" in
GLOBAL) BDF=bd_f11gl.bin ;;
CN) BDF=bd_f11cn.bin ;;
*) echo "unknown region '$hwc'. Look in /vendor/firmware_mnt/image for bd_f11*.bin and set BDF=... yourself." >&2
   [ -n "${BDF:-}" ] || exit 2 ;;
esac
echo "device raphael, region ${hwc:-?}, Wi-Fi board file $BDF"

BYNAME=/dev/block/bootdevice/by-name
mkdir -p fw-raphael out/raphael/firmware

echo "== modem firmware (read-only mount) =="
adb shell "mkdir -p /vendor/firmware_mnt && (mountpoint -q /vendor/firmware_mnt || mount -t vfat -o ro $BYNAME/modem /vendor/firmware_mnt)"
adb shell test -f /vendor/firmware_mnt/image/modem.mdt || { echo "modem.mdt not found after mounting" >&2; exit 1; }
for f in modem.mdt wlanmdsp.mbn "$BDF" $(adb shell 'ls /vendor/firmware_mnt/image' | tr -d '\r' | grep -E '^modem\.b[0-9][0-9]$'); do
	adb pull -q "/vendor/firmware_mnt/image/$f" "fw-raphael/$f"
done
ls fw-raphael | wc -l | xargs echo "modem files copied:"

echo "== NV partitions (full, read-only) =="
for part in modemst1 modemst2 fsg fsc; do
	adb exec-out "cat $BYNAME/$part" > "fw-raphael/nv-$part.tmp"
	expected=8388608
	[ "$part" = fsc ] && expected=262144
	actual=$(stat -c %s "fw-raphael/nv-$part.tmp")
	if [ "$actual" -ne "$expected" ]; then
		echo "$part is $actual bytes, expected $expected. Stop and report it; do not pad or replace it." >&2
		rm -f "fw-raphael/nv-$part.tmp"
		exit 1
	fi
	mv "fw-raphael/nv-$part.tmp" "fw-raphael/nv-$part"
	echo "  $part ok ($actual bytes)"
done

echo "== XBL (to build the UEFI for your firmware) =="
for part in xbl xbl_config; do
	adb exec-out "cat $BYNAME/$part" > "out/raphael/firmware/$part.img"
done
sha256sum out/raphael/firmware/xbl.img out/raphael/firmware/xbl_config.img

echo "== two small helper files for the Wi-Fi driver =="
BDF=$BDF python3 - <<'PYTHON'
import struct
from pathlib import Path
folder = Path('fw-raphael')
def entry(kind, data):
    return struct.pack('<II', kind, len(data)) + data + bytes(-len(data) % 4)
header = b'QCA-ATH10K\0'.ljust(12, b'\0')
meta = header + entry(1, struct.pack('<I', 0x5bbee4a4)) + entry(2, bytes.fromhex('40 00 0c'))
meta += entry(5, struct.pack('<I', 4)) + entry(6, struct.pack('<I', 3))
(folder / 'firmware-5.bin').write_bytes(meta)
(folder / '0-rmtfs-ram').write_bytes(bytes(0x310000))
PYTHON
chmod -R go-rwx fw-raphael out/raphael/firmware
echo "done. These files belong to your phone only; keep them out of git and do not share them."
