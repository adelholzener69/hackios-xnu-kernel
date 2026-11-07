# Sound on the Redmi K20 Pro: plan and verified numbers

Status: **milestone 4 done: the loudspeaker plays, and so does iOS's sound (a Redmi K20 Pro, 2026-11-07) (the amplifier starts from its container, its protection messages are delivered) (the amplifier answers over I2C; the DSP runs the speaker interface's clock) (the speaker amplifier answers over I2C: TFA9874). Milestone 3 done: the audio DSP boots, links up (GLINK, QRTR) and its APR audio services are up. The speaker path is next.** Everything in the table below was read from this phone's
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

**Milestone 3 (the link), passed on a Redmi K20 Pro, 2026-11-07.** Boot with `adsp_step=11`. After the DSP is up:

- GLINK: versions agreed 52 ms after ours; the DSP opened `glink_ssr`, `fastrpcglink-apps-dsp`, `IPCRTR`,
  `LOOPBACK_CTL_LPASS`, `apr_audio_svc` and `apr_apps2`. `IPCRTR` and `apr_audio_svc` are open both ways; the others are
  left unanswered (nothing here speaks them);
- QRTR: hello both ways; the DSP is node 5 and announced 8 services (ids 0x301, 0x42, 0x422, 0x2b, 0xf twice,
  0x33, 0x18);
- APR: **"the audio services are up" after 503 ms and one question**; the core service listed its services and
  versions (ids 0 to 0xd);
- the DSP was asked to stop and shut down cleanly.

`adsp=1` stays refused on raphael: it would also start the POCO's speaker path (`hakios.sound.c`), whose addresses are not
this phone's. `adsp_step=N` (N up to 11) is the supported way.

## The speaker path: what the phone has

From the device tree: one **NXP TFA98xx smart amplifier at I2C address 0x34** on `qupv3_se1_i2c`, reset on GPIO 59
(`0x3b`), interrupt on GPIO 141 (`0x8d`); a `fsa4480` audio switch for the USB-C port; the sound card
`sm8150-tavil-snd-card` (WCD9341 codec, not needed for the loudspeaker).

From the stock ROM (`vendor.img`): the amplifier's configuration container `firmware/mono.cnt` (12,503 bytes),
Android's routing `etc/mixer_paths_tavil.xml`, and the audio calibration `etc/acdbdata/Forte/Forte_Speaker_cal.acdb`
and `adsp_avs_config.acdb`.

**Milestone 4: the speaker path.** What Android's own configuration says (from `audio_platform_info.xml` and the device tree):

| | |
|---|---|
| Loudspeaker backend | `SND_DEVICE_OUT_SPEAKER` -> backend `speaker`, interface **`QUAT_MI2S_RX`** (the codec's own SLIMbus speaker paths in `mixer_paths_tavil.xml` are the other route and do not apply) |
| AFE port | quaternary MI2S RX (`0x1006` in the Q6 AFE numbering) |
| Port lines | one transmit data line (to the amplifier), two receive lines (the amplifier's voltage/current feedback) |
| Pins (TLMM) | GPIO 137 (clock) and 138 (word select), 139 (data to the amplifier), 140 (feedback); function `qua_mi2s`, drive 8 mA, no pull |
| Amplifier | NXP TFA98xx, I2C address `0x34` on `qupv3_se1_i2c`, reset GPIO 59, interrupt GPIO 141; container `mono.cnt` from the ROM (`fw-audio/mono.cnt`) |

Order matters: a TFA98xx locks its clock to the I2S bit clock, so the port has to be running before the amplifier can finish
starting. The sequence is therefore: TLMM pins to `qua_mi2s`; the DSP opens the MI2S port over APR; reset released and the
amplifier configured over I2C from its container; an ASM/ADM stream routed to the port; PCM written.

Sub-steps, each a separate test: (a) which TLMM tile holds GPIO 137 to 140, and the amplifier's ID over I2C (read only, after
releasing its reset); (b) open the MI2S port over APR and watch it start; (c) the amplifier's start-up from `mono.cnt`;
(d) a stream and a tone. The POCO's `hakios.sound.c` already does (b) and (d) for a TDM port, so (b) and (d) mainly swap the port
configuration; (a) and (c) are new for this phone (a TFA98xx instead of the POCO's Awinic amplifiers).

**Sub-step (a), passed on a Redmi K20 Pro, 2026-11-07** (boot-arg `amp_probe=1`, `bsd/dev/raphael_amp.c`, read only):

- the pins for GPIO 137 to 141 are in the south TLMM tile, whose registers reach 0x8d000 (the touch screen driver maps less; my first
  try faulted on exactly that);
- QUPv3 wrapper 0 serial engine 1 (`0x884000`) already has its I2C firmware loaded (protocol 3), and its clocks come up with the same
  recipe as the touch screen's (vote register `0x5200c`, bits 6, 7 and 11, clock source `0x17278` at 19.2 MHz);
- **the amplifier answers at address 0x34 and its ID register (3) reads `0x0c74`: a TFA9874.** Registers 0 and 1 read `0x0001` and `0x0000`.
  The answer is the same before and after pulsing the reset line, so the chip was already awake;
- nothing was written to the amplifier.

**Sub-step (b), passed on a Redmi K20 Pro, 2026-11-07** (`adsp_step=12`, `bsd/dev/raphael_mi2s.c`, image built with `ADSP=1`):

- GPIO 137 to 140 switched to the quaternary MI2S function (mux 1, 8 mA, no pull; clock and word select driven high first);
- the DSP accepted, each with status 0: the interface bit clock (clock id 0x106, 1.536 MHz = 48 kHz x 16 bits x 2), the port's
  I2S configuration (master, 48 kHz, 16 bits, stereo, data line 0) and the port's start (AFE port 0x1006), and later the stop;
- **the pins show it: with the port running, the bit clock (137) changed 183,162 times in 200,000 samples and the word select
  (138) 30,746 times; before, neither moved.** The data line stays still (no stream yet).

**Sub-step (c), passed on a Redmi K20 Pro, 2026-11-07** (`adsp_step=12 tfa=1`, image with `ADSP=1` and `fw-audio/mono.cnt`):

- the amplifier's start-up is NXP's own driver core (`bsd/dev/tfa98xx/`, Apache-2.0, from the phone's Android kernel) on the I2C bus
  of `raphael_amp.c`, reading the phone's `mono.cnt` (12,503 bytes: TFA9874, rev 0x0c74, family 2, no DSP of its own,
  3 profiles, the first `MUSIC_48000`);
- `tfa_dev_start(profile 0)` returned 0 with the interface's clock running; held 3 s, clock and word select still running
  (163,012 and 31,512 changes in 200,000 samples); `tfa_dev_stop` returned 0;
- **15 messages for the speaker-protection module were not delivered**: the TFA9874 has no DSP, its protection algorithm
  (SpeakerBoost) runs on the audio DSP as the module `TFADSP_RX` (0x1000B911, parameter 0x1000B921), fed back by the amplifier's
  current/voltage sense through `TFADSP_TX` (0x1000B912) on the quaternary MI2S TX port. **No sound is played until that is in place**
  (or the level is kept very low).

**Sub-step (c2), passed on a Redmi K20 Pro, 2026-11-07: the speaker-protection messages are delivered.** All 15 messages of the
container's start-up reach the audio DSP's `TFADSP_RX` module (0 refused), and `tfa_dev_start` returns 0 with the interface running:

- in band (inside the APR packet) the DSP takes the command `SET_PARAM_V3` (0x100fa: port, memory header, payload size; the
  parameter header has an instance id), but only up to about 72 bytes of message: the DSP offers the APR channel 128-byte GLINK
  intents only (the others, 512, 1152 and 8320 bytes, are on the RPC channel);
- longer messages (153 to 549 bytes: the speaker model, the configuration, the presets) go through memory of ours, as Android does for all
  of them: one page behind SMMU stream 0x1b21 (this phone's `qcom,msm-audio-ion`; `hakios.sound.c`'s code, now with the stream
  chosen per phone), mapped for the AFE service (`AFE_SERVICE_CMD_SHARED_MEM_MAP_REGIONS`, pool 3, **one 4 KB page: 64 KB did not work**),
  and the older `SET_PARAM_V2` command (the v3 command through memory is refused);
- **the DSP reads memory, not our caches**: without cleaning the lines (`dc civac`) before the command it saw stale data, and refused
  everything longer than a few bytes with "bad parameter". This cost six test runs.

**Sub-step (c3), passed on a Redmi K20 Pro, 2026-11-07** (`adsp_step=12 tfa=1 tfa_tx=1`): the protection chain is complete. The feedback
port (QUAT_MI2S_TX, 0x1007: I2S, 48 kHz, 16 bits, stereo, data line 1 = GPIO 140) is opened and started beside the playback port, the
DSP's `TFADSP_TX` module is enabled on it (0x1000b920 = 1) and `TFADSP_RX` is set to *not bypassed* (0x1000b923 = 0), all with status 0; the
15 start-up messages are delivered and the amplifier starts, the clock running throughout; at the end everything is switched off again.

**Sub-step (d), passed on a Redmi K20 Pro, 2026-11-07: THE LOUDSPEAKER PLAYS (heard by the owner).** `adsp_step=12 tfa=1 tfa_tx=1 tone=8000 tone_gain=32
tone_ramp_ms=1500`: an ADM device path and an ASM stream to port 0x1006 (the flow of `hakios.sound.c`, with the endpoint chosen per
phone), a 1 kHz tone at 32/256 of -20 dBFS (about -38 dBFS) rising over 1.5 s. 409 periods written and given back, 0 errors, 0 underflows, the
DSP took 48,134 frames a second; the amplifier's data pin carried the samples and the amplifier stayed engaged, with the DSP's speaker
protection (TFADSP, fed by the feedback port) in the path.

What it took after the amplifier and the protection were in place (three runs): the first two tones were silent although the data reached the
pin. The amplifier's registers showed why: it was engaged right after start-up (amplifier enable, PLL and clocks locked, state "operating"), and
about 50 ms later it dropped out (TDM error flagged, clocks lost). **Android's speaker is 24 bits in 32-bit slots** (`audio_platform_info.xml`:
`bit_width="24"`), so the interface runs at 3.072 MHz, and the amplifier's container is made for that frame; the 16-bit, 1.536 MHz frame I
had chosen made it flag an error and shut down. With 24 bits (AFE I2S config `bit_width` 24, bit clock 48,000 x 32 x 2, and the NXP driver told
`pcm_format` 24) it stays up and plays. Also needed: the un-mute Android's driver does after each start.

**iOS's sound plays through the speaker, smooth (a Redmi K20 Pro, 2026-11-07).** Boot (the full image, `RC_JOBS=devices/raphael/raphael-sound.jobs` for the
test session) with `adsp_step=12 adsp_keep=1 tfa=1 tfa_tx=1 ios_audio=1 ios_gain=64`: the port, the feedback port and the amplifier stay up for
the whole run, and `/dev/hakios-audio` (iOS's audio server, through `HakiosAudio.driver`) is fed to the DSP by the same feeder the POCO uses, without that
phone's guard (the DSP's TFADSP is the protection here). The tone iOS played through an audio queue, the system sounds and the owner's ringtones all
came out of the loudspeaker.

- `ios_gain` (out of 256, default 64: 12 dB under) is a digital cap of ours on top of iOS's own volume; raise it (up to 256) to taste;
- **the first version hitched on ringtones**: iOS writes at the pace the DSP plays, so the ring was nearly empty and periods often went out part-filled
  with silence. A 50 ms jitter buffer in front of the DSP (play once 50 ms are in the ring, wait again if it runs dry, let the tail out
  50 ms after iOS stops writing) fixed it: in the next run it started 3 times, never ran dry while iOS was writing, 0 underflows; the owner: "smooth";
- everything is stopped in order when the run ends (amplifier, feedback, ports, clock, pins).

Not done: iOS's volume buttons do not change the amplifier (only iOS's own scaling of the samples); the amplifier and the feedback port stay on for
the run even when nothing plays (battery); the panic path does not switch the amplifier off (`hakios_sound_quiesce` is the POCO's).

**Faster tests.** Everything up to here needs only the kernel and the small boot image (it carries `fw/adsp.*` and `mono.cnt`), not the
2.4 GB system image: boot with `fastboot boot`, skip `./xl ramdisk`, and `./xl run` with `XL_RAMDISK=userland/root.apfs`; the kernel panics at the
end for want of `launchd`, which does not matter. It saves about two minutes of each run.

This is the part most likely to take several test sessions, and the amplifier's start-up is where it is most
likely to stall.

