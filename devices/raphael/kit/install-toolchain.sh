#!/usr/bin/env bash
# Install the two things the HakiOS build takes from "Theos" -- WITHOUT Theos's installer.
#
#   devices/raphael/kit/install-toolchain.sh [install-dir]        (default: $THEOS or ~/theos)
#
# The build never uses Theos itself (no makefiles, no Swift, no ldid). It only needs:
#   1. $THEOS/toolchain/linux/iphone/bin/   Apple's cctools (ld, nm, ar, lipo, ...) plus clang, as one
#                                           86 MB tarball from github.com/L1ghtmann/llvm-project
#   2. $THEOS/sdks/iPhoneOS16.5.sdk         from github.com/theos/sdks (sparse checkout, ~260 MB)
# This script does exactly what Theos's installer does for those two, in plain steps you can also
# run by hand (see docs/RAPHAEL-K20PRO-THEOS.md). It is safe to re-run: finished parts are skipped.
set -euo pipefail

THEOS=${1:-${THEOS:-$HOME/theos}}
TAG=test-210562a                         # the release the installer's "latest" pointed at when this was tested
ARCH=$(uname -m)
case "$ARCH" in x86_64 | aarch64) ;; *) echo "no prebuilt toolchain for $ARCH" >&2; exit 2 ;; esac
TOOLCHAIN_URL=https://github.com/L1ghtmann/llvm-project/releases/download/$TAG/iOSToolchain-$ARCH.tar.xz
SDK_REPO=https://github.com/theos/sdks.git
SDK=iPhoneOS16.5.sdk

for tool in curl tar xz git; do
	command -v "$tool" >/dev/null || { echo "missing tool: $tool (install it with your package manager)" >&2; exit 2; }
done
mkdir -p "$THEOS/toolchain" "$THEOS/sdks"

echo "== 1/3 toolchain -> $THEOS/toolchain/linux/iphone"
if [ -x "$THEOS/toolchain/linux/iphone/bin/ld" ]; then
	echo "   already there"
else
	tmp=$(mktemp "${TMPDIR:-/tmp}/iOSToolchain.XXXXXX")
	trap 'rm -f "$tmp"' EXIT
	curl -fL --retry 5 --retry-delay 3 --progress-bar -o "$tmp" "$TOOLCHAIN_URL"
	echo "   downloaded $(stat -c %s "$tmp") bytes, sha256 $(sha256sum "$tmp" | cut -d' ' -f1)"
	tar -xJf "$tmp" -C "$THEOS/toolchain/"
fi

echo "== 2/3 $SDK -> $THEOS/sdks-git (sparse checkout, one directory of github.com/theos/sdks)"
if [ -f "$THEOS/sdks-git/$SDK/SDKSettings.plist" ]; then
	echo "   already there"
else
	rm -rf "$THEOS/sdks-git"
	git clone -q --depth 1 --filter=blob:none --sparse "$SDK_REPO" "$THEOS/sdks-git"
	git -C "$THEOS/sdks-git" sparse-checkout set "$SDK"
fi
ln -sfn "../sdks-git/$SDK" "$THEOS/sdks/$SDK"

echo "== 3/3 check"
fail=0
ld_out=$("$THEOS/toolchain/linux/iphone/bin/ld" -v 2>&1 || true)    # ld -v exits non-zero: judge by what it prints
case "$ld_out" in
*PROGRAM:ld*) echo "   ld runs:  $(echo "$ld_out" | head -1)" ;;
*)
	echo "   ld does NOT run. Most often a missing library: install ncurses (libtinfo.so.6; Debian/Ubuntu: libtinfo6)." >&2
	echo "$ld_out" | head -3 >&2
	fail=1
	;;
esac
[ -f "$THEOS/sdks/$SDK/SDKSettings.plist" ] && echo "   SDK ok:  $THEOS/sdks/$SDK" || { echo "   SDK missing" >&2; fail=1; }
[ "$fail" = 0 ] || exit 1
echo
echo "done. Use it with:   export THEOS=\"$THEOS\""
