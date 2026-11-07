# Troubleshooting

Each entry is something that actually happened while bringing this up.

## Black screen after `fastboot boot`, and `xnuloader` never appears
The upstream release image `raphael-uefi.img` embeds the XBL drivers of the *author's* phone. On a
phone whose XBL is a different build it starts, then stops. Use `devices/raphael/kit/build-uefi-image.sh`,
which builds the UEFI from **your** XBL. Run `devices/raphael/kit/check-xbl.py` first; it says whether the three
patched modules match.

## Crash screen: `Data abort: Translation fault, third level`, `FAR 0x09020008`
The loader decided it was running under QEMU and probed QEMU's `fw_cfg` device, which does not exist
on a phone. That is the bug commit `84647ba` fixes (the framebuffer check only knew the POCO X5 Pro's
address). Do not "fix" it by mapping that address in the UEFI memory map: the probe then hangs the
bus instead of crashing. If you still see it, you are not on the `raphael-k20pro` branch (`git checkout raphael-k20pro`).

## The loader banner `XNULOADER v3 … CurrentEL=1` is the last line and nothing happens
Same cause as above (a hang instead of a crash). Check you are on the `raphael-k20pro` branch.

## `ConvertPages: Incompatible memory types, the pages to allocate have been allocated`
Harmless. The loader asks for its preferred address (`0xA1000000`), which is reserved, so the UEFI
relocates it, followed by `Loading driver at 0x…`. It is not the failure if the banner follows.

## fastboot says `Write to device failed`, or the phone keeps disappearing and reappearing
A flaky USB link (the device number in `lsusb` keeps climbing). Use a USB port on the back of the
computer, no hub or front panel, and a short plain cable. It happens during `fastboot boot` and during
the 2.5 GB upload; retry once the link is stable.

## `xl: loader did not come up on USB (state: fastboot)` or `xl: a run is in progress`
`xl` allows one run at a time. A run lasts `XL_SECS` (600 s by default) and the phone answers as
`fastboot` while it runs. Do not start a second `./xl run` while one is active. If `xl` cannot find the
loader it builds and boots a fresh image itself; make sure the device settings (`UEFI_TARGET=raphael`
etc.) are the ones `devices/raphael/kit/build-uefi-image.sh` uses, so it never builds an image for
another phone.

## After a run, do not reuse the loader: restart the phone
Once a run has returned (or been stopped), the loader is not reliable any more: starting a new upload
right away failed and froze the phone, and a loader left idle for a long time was found reset to the
stock bootloader. Between sessions, hold Power + Volume Down to restart into fastboot, then repeat
`fastboot boot` -> `./xl ramdisk` -> `./xl run`. (Do this even if `./xl state` still says `loader`.)

## The phone froze, or I rebooted it
The 2.5 GB image lives in RAM, so after any reboot start again from `fastboot boot` and upload it
again (about 100 s). Do not reboot during a run. Start the upload and the run soon after the loader
comes up; do not leave the loader sitting idle.

## Wi-Fi shows no networks for a while
Expected: the driver is early. Give it a minute or two. After turning Wi-Fi off and on it often fails
with `failed to power on device: -19`; that is a known upstream bug, and ending the session and
starting a new one fixes it.

## Android will not boot afterwards
Nothing but the (optional) TWRP recovery is ever written. A normal restart goes to Android as before.
To put the stock recovery back, use `images/recovery.img` from your own ROM with
`fastboot flash recovery recovery.img`. Do not flash `xbl`, `abl` or other firmware.
