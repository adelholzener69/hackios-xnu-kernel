# Sound on the Redmi K20 Pro: plan and verified numbers

Status: **planning, nothing implemented yet.** Everything in the table below was read from this phone's
own device tree (`raphael` DTB built by `devices/raphael/kit/build-raphael-dtb.py`) or its ROM; the
"POCO" column is what the existing audio code for the POCO X5 Pro (`bsd/dev/hakios.adsp.c`) uses.

## What has to happen

The phone's audio hardware belongs to the **audio DSP** (`adsp`, a Hexagon inside the SoC), not to the
main processor. It stays in reset until someone loads its signed firmware, so sound is built in layers:

1. Boot the DSP: write its firmware into reserved memory, ask TrustZone to authenticate and start it.
2. Talk to it: SMP2P ("is it up"), GLINK (channels), QRTR on the channel `IPCRTR` (what services it offers).
3. APR on the channel `apr_audio_svc` (the audio services: AFE, ADM, ASM).
4. The speaker path: route a stream to the MI2S/TDM port and switch on the amplifiers.
5. The iOS side (`HakiosAudio.driver`, `hakios.sound.c`): already generic, it just feeds PCM to `/dev/hakios-audio`.

Steps 1 to 3 are the DSP bring-up and are mostly the same code on both phones. Steps 4 and 5 are device-specific.

## The numbers (SM8150 / raphael)

| | POCO X5 Pro (code today) | raphael (from the DTB) |
|---|---|---|
| Doorbell mechanism | IPCC (`0x00408000`) | APCS mailbox, register `0x17c0000c`, one bit per signal |
| GLINK doorbell | IPCC client 3, signal 0 | **bit 8** (`qcom,glink adsp`: `mboxes = <apss_shared 8>`) |
| SMP2P doorbell | IPCC client 3, signal 2 | **bit 10** (`qcom,smp2p-adsp`: `qcom,ipc = <apcs 0 10>`) |
| Remote processor id | 2 | **2** |
| SMP2P shared-memory items | 443 (theirs), 429 (ours) | **443 / 429** (`qcom,smem = <443 429>`), same |
| Last-words item | 423 | **423** (`qcom,smem-id = <423>`), same |
| GLINK label | `lpass` | **`lpass`** |
| TrustZone image id | 1 | **1** (`qcom,pas-id = <1>`), same |
| DSP memory | `0x86700000`, 64 MB | **`0x8be00000`, `0x2200000` (36 MiB)** (`reserved-memory pil_adsp_region`) |
| GLINK interrupt / SMP2P interrupt | n/a (IPCC) | SPI 156 / SPI 158 (polling is enough to start) |
| Firmware | `adsp.mdt`, `adsp.b02..b36` | **`adsp.mdt` + `adsp.b00..b13`** in the modem partition image (`/vendor/firmware_mnt/image`), 15 files |

So the same structure applies; what differs is the doorbell mechanism (the raphael Wi-Fi code
already has this: `hakios.wifi.c` `ring()` writes bits of `0x17c0000c` for the modem, bit 12 GLINK and bit 14
SMP2P), the memory region, and the firmware layout.

## Plan

1. **Firmware.** Add `adsp.mdt` and `adsp.b*` to `devices/raphael/kit/pull-firmware.sh` and to the image
   packaging next to the modem files.
2. **Diagnostics only (steps 1 and 2 of the POCO driver, read only).** Read the shared memory partition
   with the DSP, check the firmware is whole and fits the 36 MiB region. Nothing is written to hardware.
3. **Bring-up.** Boot the DSP (TrustZone image 1), watch SMP2P report "up", open GLINK and QRTR, list the
   services it offers. Rails (`lcx`, `lmx`) are requested through RPMh and the always-on processor is told
   "adsp: on", as the modem path does for its own rails.
4. **APR audio services** and a first tone to the speaker path.
5. **Speaker amplifiers (NXP TFA98xx).** Need the phone's own amplifier configuration file (found in the
   ROM's vendor image; not yet located) and an I2C bring-up.

## Open questions

- Where the **ambient light and other sensors** run (no `slpi` firmware in the modem image; probably the
  audio DSP). The audio DSP bring-up decides this, and auto-brightness depends on it.
- The speaker amplifiers' exact I2C addresses, reset lines and configuration.
- Whether the iOS side's expected audio topology matches the one this DSP's firmware offers.

## What this will not do (at first)

Headphones, microphones, calls and Bluetooth audio all need the SLIMbus codec (WCD9341) and are not part
of the first goal, which is sound from the loudspeaker.
