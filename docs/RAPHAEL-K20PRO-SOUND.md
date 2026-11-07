# Sound on the Redmi K20 Pro: plan and verified numbers

Status: **milestone 2 done: the audio DSP boots on the phone and reports "up". The link (GLINK, QRTR) and APR are next.** Everything in the table below was read from this phone's
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

## Progress

**Milestone 1 (read only), passed on a Redmi K20 Pro, 2026-11-07.** Build the image with the firmware
(`ADSP=1`, firmware in `fw-audio/`; `devices/raphael/kit/pull-firmware.sh` or the ROM's `NON-HLOS.bin` provide it)
and boot with `adsp_step=2`. The kernel log then says:

- the shared memory is at `0x86000000`; the partition between the main processor and the audio DSP exists
  (`0x861cf000`, 0x30000 bytes, 0 items), and the DSP has not run (item 443 absent);
- the image is whole: a header of 7820 bytes (7288 of hashes), 13 segments, 14,244,900 bytes of files, covering
  exactly `0x2200000` bytes from `0x8be00000`;
- nothing was written to hardware.

Found on the way: GLINK's descriptor and our queue (shared-memory items 478/479) are absent on this boot chain;
the POCO's boot chain creates them. The raphael Wi-Fi code does it itself (`glink_prepare` in `hakios.wifi.c`), so
the audio DSP needs the same.

**Milestone 2 (the writes), passed on a Redmi K20 Pro, 2026-11-07.** Boot with `adsp_step=7`. Steps 3 to 7 ran:

- the always-on processor's mailbox opened (doorbell: APCS bit 0);
- TrustZone accepted the firmware header (image 1) and took the 36 MiB region;
- all 13 segments written (14,244,900 bytes, 21 MB cleared), read back with 0 differences. One segment is 0x54
  bytes long, so the writer pads the last 8-byte word with zeros;
- our SMP2P item (429) and GLINK's descriptor and queue (478/479) made in our partition with the DSP (raphael's boot
  chain makes none of them);
- CX at its turbo position and the crystal oscillator voted (the audio DSP's proxy votes on SM8150, kept until restart);
  the always-on processor told "adsp: on"; TrustZone authenticated the image and released the DSP from reset;
- **the DSP's SMP2P item appeared 8 ms later and it said "up" (`slave-kernel = 0x6`) after 13 ms**, having asked for
  its own power; it made its own GLINK queue (480) and two more items (606, 611);
- shut down cleanly: it acknowledged the stop in 1.9 ms, TrustZone shut it down, the always-on processor was told "off".

What changed in the code (`hakios.adsp.c`, plus one exported helper in `hakios.wifi.c`): the doorbells (`ring_dsp`,
`ring_aop`: APCS bits 8, 10 and 0), polling instead of IPCC, the shared memory at `0x86000000`, the DSP's memory,
`item_make()` and `glink_prepare()`, and the proxy votes (`hakios_raphael_cx_xo_proxy()`).

**Next, milestone 3:** the link. GLINK's channels, QRTR on `IPCRTR`, and APR on `apr_audio_svc`, to see whether the
audio services come up (steps 8 to 11, `adsp_step=11`). `adsp=1` stays refused on raphael: it would also start the
POCO's speaker path, whose addresses are not this phone's.
