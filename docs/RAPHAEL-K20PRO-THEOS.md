# Getting the iOS toolchain (the "Theos" part) without Theos's installer

The HakiOS build asks for "Theos". **It does not use Theos.** No makefiles, no Swift, no `ldid`, none
of the installer's other steps. It needs exactly two things, and only these two:

| What | Where the build looks | Size |
|---|---|---|
| Apple's `cctools` (`ld`, `nm`, `ar`, `lipo`, ...) plus a clang, as one prebuilt tarball | `$THEOS/toolchain/linux/iphone/bin/` | 86 MB download, about 590 MB unpacked |
| The iPhoneOS 16.5 SDK | `$THEOS/sdks/iPhoneOS16.5.sdk` | about 260 MB |

`$THEOS` is just a folder name (default `~/theos`). If Theos's one-line installer failed for you, skip
it completely and use one of the two ways below. About 1 GB of disk in total.

## Way 1: the script

```sh
devices/raphael/kit/install-toolchain.sh            # installs into ~/theos
devices/raphael/kit/install-toolchain.sh ~/mytools  # or anywhere else
```

It runs the commands of Way 2, skips what is already there, and ends with a check
(`ld runs: ...`, `SDK ok: ...`). It needs `curl tar xz git` and, to run the toolchain, the ncurses
library (see [if it fails](#if-something-fails)).

## Way 2: by hand

These are the same steps the installer performs for these two pieces, written out. Pick your folder first:

```sh
export THEOS="$HOME/theos"
mkdir -p "$THEOS/toolchain" "$THEOS/sdks"
```

**The toolchain** (x86_64 shown; on an ARM PC use `iOSToolchain-aarch64.tar.xz`):

```sh
curl -fL --retry 5 -o iOSToolchain.tar.xz \
  https://github.com/L1ghtmann/llvm-project/releases/download/test-210562a/iOSToolchain-x86_64.tar.xz
tar -xJf iOSToolchain.tar.xz -C "$THEOS/toolchain/"
```

That creates `$THEOS/toolchain/linux/iphone/`. The tarball tested here is 86,163,280 bytes with SHA-256
`a72a7a577e2fbe2838b6b5e9c72034fa7d114af96f0e1d4b016f18730ce4056e` (`sha256sum iOSToolchain.tar.xz`).
You can delete the `.tar.xz` afterwards. A slow or broken download resumes with `curl -C - ...`.

**The SDK** (a sparse clone, so you fetch one folder, not the whole repository):

```sh
git clone --depth 1 --filter=blob:none --sparse https://github.com/theos/sdks.git "$THEOS/sdks-git"
git -C "$THEOS/sdks-git" sparse-checkout set iPhoneOS16.5.sdk
ln -sfn ../sdks-git/iPhoneOS16.5.sdk "$THEOS/sdks/iPhoneOS16.5.sdk"
```

## Check it

```sh
"$THEOS/toolchain/linux/iphone/bin/ld" -v 2>&1 | head -2      # expect: PROGRAM:ld  PROJECT:ld64-609
test -f "$THEOS/sdks/iPhoneOS16.5.sdk/SDKSettings.plist" && echo "SDK ok"
```

Then use it for the build with `export THEOS="$HOME/theos"` (or your folder). **Do not add
`$THEOS/toolchain/linux/iphone/bin` to your `PATH` yourself.** Its `ld` is a macOS-style linker that
would replace your system linker and break unrelated builds; HakiOS's scripts call it by full path.

## If something fails

- **`ld` prints `error while loading shared libraries: libtinfo.so.6`** (or similar). Install ncurses:
  Debian/Ubuntu `sudo apt install libtinfo6`, Arch `sudo pacman -S ncurses`, Fedora `sudo dnf install ncurses-libs`.
- **The `curl` download stops or is very slow.** Add `-C -` to resume. If GitHub downloads are blocked
  where you are, download the file in a browser from the same URL and unpack it as shown above.
- **`git clone` of the SDK is slow.** It is about 260 MB. The `--filter=blob:none --sparse` form is
  deliberate; do not clone the whole `theos/sdks` repository.
- **`tar: ... xz: not found`**: install `xz` (Debian/Ubuntu `xz-utils`).
- **The release file disappeared.** The URL is pinned to the release the installer used when this
  was tested. If it is ever removed, the only other route I know is building Apple's `cctools` yourself
  ([tpoechtrager/cctools-port](https://github.com/tpoechtrager/cctools-port)). That is **untested** here;
  please open an issue if you have to take it.
- **Not on x86_64 or aarch64 Linux?** There is no prebuilt toolchain for other architectures.

## What was tested

On x86_64 Linux: the toolchain tarball was downloaded from the URL above, unpacked into an empty folder,
the SDK sparse-checked-out with the commands above, and `ld` ran (`ld64-609`, built September 2023). The
script's final check was fixed after
that run and has only been syntax-checked since. The same `ld` build date and version string appear in
the toolchain that was already working on the tested machine, so it is the same toolchain.
