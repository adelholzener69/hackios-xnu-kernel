# HakiOS on the Redmi K20 Pro / Mi 9T Pro (`raphael`)

This is the `raphael-k20pro` branch of [HakiOS](https://github.com/HeliumStudio-Dev/hackios-xnu-kernel): Apple's open-source
XNU kernel (xnu-8796.121.2) running **iOS 16.5** on an Android phone, here the **Redmi K20 Pro / Mi 9T Pro** (Snapdragon 855, `raphael`).

It is upstream's `raphael-bringup` port plus changes that make it boot on phones whose firmware differs from the original author's,
helper scripts that build the UEFI **from your own phone's firmware** (`devices/raphael/kit/`), and the work to bring the phone
up to the level of HakiOS's main device, the POCO X5 Pro.

> **Experimental.** A research port, not a usable phone OS. Everything runs from RAM: nothing is installed, and a restart returns the
> phone to how it was. Use a spare phone. **No firmware, no Apple files and no built images are in this repository, or ever should be**
> (see [LEGAL.md](LEGAL.md) and [COMPLIANCE.md](COMPLIANCE.md)).

**Start here: [docs/RAPHAEL-K20PRO.md](docs/RAPHAEL-K20PRO.md)**, the step-by-step quickstart (unlocked bootloader and a Linux PC needed).

## What works (tested on one Redmi K20 Pro, 6 GB, Global, firmware V12.5.2.0)

| | |
|---|---|
| Display, GPU (Adreno 640, OpenGL ES) | yes, hardware rendering |
| Touch, power and volume buttons | yes |
| CPU | 7 of 8 cores, frequency scaling |
| Wi-Fi | yes, but slow to find networks, and no reconnect after switching it off and on |
| Battery level and charging state | yes (read-only) |
| Brightness slider | yes, the panel's own brightness |
| **Sound** | **yes: the loudspeaker plays iOS's sound**, through the audio DSP with its speaker protection ([how](docs/RAPHAEL-K20PRO-SOUND.md)); headphones and microphones: no |
| Bluetooth, cellular, cameras, torch, haptics | no |
| Saving anything | no (RAM only) |

Details and the roadmap: [docs/RAPHAEL-K20PRO-STATUS.md](docs/RAPHAEL-K20PRO-STATUS.md).

## Documentation

| | |
|---|---|
| [docs/RAPHAEL-K20PRO.md](docs/RAPHAEL-K20PRO.md) | Quickstart: from a stock phone to iOS on the screen |
| [docs/RAPHAEL-K20PRO-STATUS.md](docs/RAPHAEL-K20PRO-STATUS.md) | What works, what does not, the order of work |
| [docs/RAPHAEL-K20PRO-SOUND.md](docs/RAPHAEL-K20PRO-SOUND.md) | How the sound was brought up, step by step, with the numbers |
| [docs/RAPHAEL-K20PRO-THEOS.md](docs/RAPHAEL-K20PRO-THEOS.md) | The iOS toolchain and SDK without Theos' installer |
| [docs/RAPHAEL-K20PRO-TROUBLESHOOTING.md](docs/RAPHAEL-K20PRO-TROUBLESHOOTING.md) | When something does not work |
| [README.raphael.zh.md](README.raphael.zh.md) | Upstream's original (Chinese) test guide for raphael |
| [README.zh.md](README.zh.md), [README.uk.md](README.uk.md) | HakiOS's general README in Chinese and Ukrainian |

## Safety

Never flash `xbl` or `abl` (an anti-rollback fuse can brick the phone). Everything described here runs from RAM through
`fastboot boot`. The built firmware, NV and Apple files that the scripts pull from *your* phone stay on your machine.

## Credits and licence

Upstream HakiOS and the `raphael-bringup` port: [HeliumStudio-Dev](https://github.com/HeliumStudio-Dev/hackios-xnu-kernel). XNU is Apple's open-source
kernel; third-party code keeps its own licences ([THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)), including NXP's TFA98xx amplifier
code under the Apache License 2.0 (`xnu-src` patch files in `xnu/files/bsd/dev/tfa98xx/`, see the README there).
