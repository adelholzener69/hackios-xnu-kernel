#!/usr/bin/env bash
# Take the Adreno 640 GPU firmware out of YOUR phone's stock ROM (its vendor.img), offline.
#
#   devices/raphael/kit/extract-gpu-firmware.sh /path/to/rom/images [path/to/this/repo]
#
# The six files come from the same firmware your phone runs, which is what the HakiOS GPU
# driver needs (a640_*; the a660_* files of other phones must not be used).
# Needs: simg2img (android-tools / android-sdk-libsparse-utils) and 7z (7-Zip 23 or newer).
# Writes <repo>/fw-gpu/ with owner-only permissions. Never publish these files.
set -euo pipefail

IMAGES=${1:?usage: extract-gpu-firmware.sh /path/to/rom/images [path/to/this/repo]}
HAKIOS=${2:-$(cd "$(dirname "$0")/../../.." && pwd)}
[ -f "$IMAGES/vendor.img" ] || { echo "no vendor.img in $IMAGES" >&2; exit 2; }
command -v simg2img >/dev/null || { echo "simg2img not found" >&2; exit 2; }
command -v 7z >/dev/null || { echo "7z not found" >&2; exit 2; }
umask 077

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
simg2img "$IMAGES/vendor.img" "$work/vendor.raw"
mkdir -p "$HAKIOS/fw-gpu"
for name in a640_gmu.bin a630_sqe.fw a640_zap.mdt a640_zap.b00 a640_zap.b01 a640_zap.b02; do
	7z e -y -o"$HAKIOS/fw-gpu" "$work/vendor.raw" "firmware/$name" >/dev/null
	[ -s "$HAKIOS/fw-gpu/$name" ] || { echo "missing $name in vendor.img" >&2; exit 1; }
done
ls -l "$HAKIOS/fw-gpu"
sum=$(sha256sum "$HAKIOS/fw-gpu/a640_gmu.bin" | cut -d' ' -f1)
echo "a640_gmu.bin sha256 $sum"
case "$sum" in 3ff0c02708bbe786*) echo "GMU firmware matches the one the Helium A640 notes document." ;;
*) echo "NOTE: GMU firmware differs from the 3ff0c027... build seen on V12.5.2.0; probably another ROM version." ;; esac
