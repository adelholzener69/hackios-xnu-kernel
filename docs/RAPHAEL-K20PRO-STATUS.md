# Status and roadmap

Tested on one Redmi K20 Pro (`raphael`, 6 GB, Global, firmware V12.5.2.0), November 2026.

HakiOS's main phone is the **POCO X5 Pro**. Its README lists these hardware drivers: display,
Goodix touch, UFS storage, Adreno 642L GPU, WCN6750 Wi-Fi, the audio DSP, the modem, Awinic
haptics and the cameras. The goal of this branch is to bring raphael up to the same level. This is where it
stands:

| Feature | POCO X5 Pro (main device) | raphael now | What it takes |
|---|---|---|---|
| Display, scanout | yes | **yes** | done |
| Touch | yes | **yes** | done (Goodix GT9886) |
| Buttons (power, volume) | yes | **yes** | done |
| CPU, frequency scaling | yes | **yes** (7 of 8 cores; core 0 stays with the loader) | done |
| GPU (OpenGL ES) | yes (Adreno 642L) | **yes** (Adreno 640) | done; Metal is not available on either |
| Wi-Fi | yes (WCN6750) | **yes** (WCN3990, ath10k) | works; slow to find networks; no reconnect after toggling Wi-Fi (known upstream bug) |
| Battery level, charging state | yes | **yes** | read-only, via the firmware's Qualcomm charger drivers (commit `84647ba`) |
| Brightness control | not found in the repo | **yes** (panel's own brightness) | done: the slider's level is caught in `backboardd`, passed to the kernel (`kern.hakios.brightness`) and sent to the Samsung EA8076 as DCS `0x51` through the DSI controller. Boot arguments `brightness=1 brightness_dcs=1`. Auto-brightness and the ambient-light sensor are not supported |
| **Sound** | yes (DSP + Awinic amps) | **no** | speaker-only path first. The POCO port's audio code is step-by-step: boot the audio DSP through TrustZone, open its GLINK link, QRTR, then APR to the audio services (`bsd/dev/hakios.adsp.c`, `hakios.sound.c`); the same APR protocol applies to your SM8150. What changes is device-specific: the DSP's memory and firmware (`adsp.mdt` + 14 segments, present in the modem partition image), the SMP2P/GLINK numbers, the doorbell (SM8150 uses the APCS mailbox, not IPCC) and the speaker path (NXP TFA98xx amplifiers, which need the phone's amp configuration file). Headphones and microphones need the SLIMbus codec (WCD9341), which is much harder |
| **Auto-brightness** | not found in the repo | **no** | needs the ambient light sensor. It is not on a bus the main processor can read; sensors on Qualcomm phones sit behind a DSP. No `slpi` firmware is in the modem image (it may run on the audio DSP instead), so this waits until the audio DSP is up. iOS's own auto-brightness needs calibration data, so a simple curve of our own, feeding the same brightness path, is the realistic plan |
| **Bluetooth** | not enabled | **no** | WCN3990 over UART; the open problem is iOS's Bluetooth stack, which no HakiOS port provides |
| Storage (keep data between boots) | yes (UFS) | **no** | storage driver for raphael |
| Haptics, torch | yes | **no** | raphael's vibration motor and flash drivers |
| Cellular | yes | **no** | modem bring-up (the NV partitions pulled in step 3 are already the first ingredient) |
| Cameras | yes | **no** | camera sensor and ISP drivers |

## Order of work

1. Speaker-only sound.
2. Haptics and torch.
3. Storage persistence.
4. Cellular, then cameras.

Bluetooth stays a research item until someone has an approach for iOS's side.

## Why one firmware version

The UEFI is built from drivers extracted from your phone's own XBL, plus a few single-byte patches
in three modules (`DALSYSDxe`, `UsbConfigDxe`, `ButtonsDxe`). On V12.5.2.0 those three modules are
byte-identical to the originals the upstream author patched, so the patches apply, even though the
XBL image as a whole differs. Another firmware version needs `devices/raphael/kit/check-xbl.py` to say so first. The published upstream UEFI image embeds the *author's* XBL
and black-screens on a phone whose XBL differs, which is the main reason this branch exists.
