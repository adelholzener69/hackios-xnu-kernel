# Quickstart: iOS 16 on the Redmi K20 Pro / Mi 9T Pro (`raphael`)

This branch (`raphael-k20pro`) runs **iOS 16.5 with a working GPU, touch and Wi-Fi** on the Redmi K20 Pro
(Snapdragon 855). It is HakiOS's `raphael-bringup` port plus fixes that let it boot on phones whose
firmware differs from the original author's, and helper scripts in `devices/raphael/kit/` that build
the UEFI **from your own phone's firmware**. **No firmware, no Apple files and no built images are in
this repository or ever should be** (see [Safety](#safety-and-legal)).

> Experimental. This is a research port, not a usable phone OS. Everything runs from RAM: nothing
> is installed, and a reboot returns the phone to how it was. Tested on one phone (see below).

## What works

| | |
|---|---|
| GPU (Adreno 640, OpenGL ES) | yes, hardware rendering |
| Touch, power and volume buttons | yes |
| CPU | 7 of 8 cores (core 0 stays with the loader), frequency scaling |
| Wi-Fi | yes (upstream verified 5 GHz WPA3), but slow to find networks, and it will not reconnect after you switch Wi-Fi off and on (restart the session) |
| Battery level and charging state | yes, read-only, taken from the phone's own firmware |
| Brightness slider | **no** (planned) |
| Sound, Bluetooth, cellular, cameras, torch, haptics | **no** |
| Saving anything | **no**: RAM only, every start is a fresh iOS |

Details, and how this compares with HakiOS's main phone (POCO X5 Pro): [docs/RAPHAEL-K20PRO-STATUS.md](RAPHAEL-K20PRO-STATUS.md).

**Tested on:** Redmi K20 Pro, 6 GB RAM, Global, stock firmware **V12.5.2.0 (Android 11)**, bootloader
unlocked. Other firmware versions may work: step 5 tells you before you build anything.

## What you need

- A Redmi K20 Pro / Mi 9T Pro, **codename `raphael`**, with an **unlocked bootloader** (unlocking
  wipes the phone; do it first). Charged battery, a **good USB cable plugged into a rear-panel
  port** (no hubs; a flaky link was the main source of trouble).
- A Linux PC with about **80 GB free disk, 16 GB RAM** and a few hours. Install: `git curl python3
  python3-venv clang lld llvm dtc adb fastboot simg2img 7zip` (7-Zip 23 or newer), plus
  `make gcc flex bison m4 perl autoconf automake libtool cmake meson ninja pkg-config xz unzip`.
- [Theos](https://theos.dev/docs/installation-linux) in `~/theos` with the **iPhoneOS 16.5 SDK**
  (HakiOS's README explains where to get it).
- Your phone's **stock fastboot ROM** for your region (from Xiaomi), unpacked: you need its
  `images/dtbo.img` and `images/vendor.img`.
- The official **TWRP for raphael** (`twrp-3.7.1_12-1-raphael.img` from twrp.me), only to read your
  modem partitions for Wi-Fi. Skip Wi-Fi and you can skip TWRP.

## Step by step

**1. Get the source.**
```sh
git clone -b raphael-k20pro https://github.com/adelholzener69/hackios-xnu-kernel hakios-raphael
cd hakios-raphael
```
Every command below is run from this folder.

**2. Install TWRP (only for Wi-Fi).** `fastboot boot twrp.img` did not work on the tested phone,
so it was flashed to the recovery partition (this replaces the stock recovery; the stock one is in
your ROM's `images/recovery.img` if you want it back). Boot to bootloader with Power + Volume Down:
```sh
fastboot flash recovery twrp-3.7.1_12-1-raphael.img
```
Then boot it with Power + Volume Up. If it asks, keep it **read-only** and cancel any password.

**3. Copy the firmware files off your phone** (TWRP running, `adb` connected). Read-only. The
results are private to your phone: never publish them.
```sh
devices/raphael/kit/pull-firmware.sh .
```

**4. Extract the GPU firmware from your ROM.**
```sh
devices/raphael/kit/extract-gpu-firmware.sh /path/to/rom/images .
```

**5. Check your firmware is compatible.** The UEFI needs three patched modules from your phone's
XBL; this says whether they match:
```sh
python3 -m venv out/mu-venv && out/mu-venv/bin/pip install -q uefi_firmware
out/mu-venv/bin/python devices/raphael/kit/check-xbl.py out/raphael/firmware/xbl.img \
    devices/raphael/uefi/raphael-firmware-patches.json
```
`VERDICT: compatible` means go on. `NOT compatible` means those patches would need re-validating
for your firmware: stop and open an issue with the output.

**6. Make the device tree** (the SM8150 base tree plus the raphael overlay, as the bootloader does):
```sh
python3 devices/raphael/kit/build-raphael-dtb.py \
    --boot-image /path/to/twrp-3.7.1_12-1-raphael.img --dtbo /path/to/rom/images/dtbo.img \
    -o out/raphael/raphael.dtb
```

**7. Build the kernel** (about 10 minutes after the dependencies):
```sh
export THEOS="$HOME/theos" XNU_SRC="$PWD/xnu-src/xnu-8796.121.2-raphael"
mkdir -p out/user-bin && export PATH="$PWD/out/user-bin:$PWD/xnu-deps/host/bin:$HOME/.local/venvs/meson/bin:$PATH"
xnu/apply.sh "$XNU_SRC" && xnu-deps/setup.sh && devices/raphael/build.sh kernel
```

**8. Build the iOS image** (downloads iOS 16.5 pieces from Apple's public servers, about 8 GB):
```sh
sysimage/fetch.sh
export MPSS_FW="$PWD/fw-raphael" WIFI_BDF=bd_f11gl.bin   # Global; use bd_f11cn.bin for CN; `unset` both for no Wi-Fi
FLASHBOOT=prepared SYSTEM=1 devices/raphael/build.sh images
mkdir -p out/raphael && cat userland/flashboot.apfs userland/system.apfs > out/raphael/ram-resident.apfs
```

**9. Build the boot image** (UEFI and loader for your firmware):
```sh
devices/raphael/kit/build-uefi-image.sh .
```

**10. Run it.** Phone in bootloader (Power + Volume Down), then:
```sh
fastboot boot out/raphael/boot.img
fastboot getvar product            # must say: xnuloader  (wait a few seconds)
./xl ramdisk out/raphael/ram-resident.apfs         # about 100 s, do not touch the cable
export XL_KERNEL="$XNU_SRC/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837" XL_RAMDISK="$PWD/userland/root.apfs"
export XL_SECS=600 XL_FOLLOW=15 XL_FOLLOW_KEEP=1
WIFI_ARGS="wifi_step=15 wifi_lx=2 wifi_ba=1 wifi_wide=1 wifi_new=99 wifi_pause=0 wifi_ask=1 wifi_bdf=$WIFI_BDF wifi_smmu=2 wifi_keep=1 wifi_wait=10 wifi_listen=15"
export XL_CMDLINE="gic_timer_irq=19 bootcpu=7 cpus=8 touchscreen=1 buttons=1 cpufreq=1 gpu=1 fb_scan=1 modem=0 adsp=0 ufs=0 ufs_write=0 var_store=0 root_apfs=md0,c=1 battery=1 $WIFI_ARGS"
./xl run
```
You should see a small triangle (the GPU self-test) and then the iOS home screen. Connect to Wi-Fi in
Settings (give it a minute to list networks). For no Wi-Fi, drop `$WIFI_ARGS` and add `wifi=0`.
The session ends by itself after 10 minutes (`XL_SECS`); `./xl stop` ends it early.

## If something goes wrong

See [docs/RAPHAEL-K20PRO-TROUBLESHOOTING.md](RAPHAEL-K20PRO-TROUBLESHOOTING.md). The common ones: a black screen right after
`fastboot boot` (firmware mismatch: build the UEFI with `devices/raphael/kit/build-uefi-image.sh`, not the release one), a crash screen at
address `0x09020008` (the loader bug fixed by commit `84647ba`), the USB link dropping, and starting a
second `xl run` while one is active.

## What is in this branch

| | |
|---|---|
| `devices/raphael/kit/` | the helper scripts used above: `pull-firmware.sh`, `extract-gpu-firmware.sh`, `check-xbl.py`, `build-raphael-dtb.py`, `build-uefi-image.sh` |
| `docs/RAPHAEL-K20PRO-STATUS.md` | what works, and the roadmap to the POCO X5 Pro's level |
| `docs/RAPHAEL-K20PRO-TROUBLESHOOTING.md` | things that went wrong while bringing this up, and fixes |
| commit `84647ba` | loader recognises raphael's framebuffer (it fell into a QEMU-only path and hung); enables the battery reader |
| commit `7089c2c` | UEFI module extractor accepts another XBL build when the three patched modules match |

**How much was verified.** Every step above was carried out by hand on the tested phone, in this
order. `check-xbl.py`, `build-raphael-dtb.py` (byte-identical to the hand-made file) and
`extract-gpu-firmware.sh` were re-run from scratch as tests. `pull-firmware.sh` and
`build-uefi-image.sh` are transcriptions of the commands used and are syntax-checked, but have not
yet been run end to end as scripts. Please report anything that differs.

## Safety and legal

- **Nothing is flashed** except the optional TWRP recovery. The boot is `fastboot boot` (RAM only).
  **Never flash `xbl`, `abl` or other firmware to "fix" a problem**: Xiaomi's anti-rollback can make
  that permanent.
- Your `fw-raphael/`, `fw-gpu/` and `out/` folders contain **your phone's private data and vendor
  firmware** (modem calibration, NV partitions) and Apple software. **Do not upload or share
  them**, and do not share logs without checking them first. The repository's `.gitignore` keeps them out.
- This project is not affiliated with Apple, Xiaomi or Qualcomm. You must supply, and be licensed to
  use, your own firmware and iOS files. Read [LEGAL.md](../LEGAL.md).
  It is not legal advice.
- Licenses are those of this repository: the kernel parts are under the Apple Public Source License,
  everything else under MIT (see `LICENSE`).

## Credits

[HakiOS](https://github.com/HeliumStudio-Dev/hackios-xnu-kernel) and its raphael port (kernel, loader,
userland, the Wi-Fi and GPU work), [Project Silicium / Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)
(UEFI), Apple's open-source XNU, and TWRP. This branch adds the fixes and scripts that made it boot on a
different firmware build.
