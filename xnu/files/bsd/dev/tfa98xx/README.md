# NXP TFA98xx amplifier code

`inc/*.h` and `src/tfa_init.c`, `src/tfa_dsp.c`, `src/tfa_container.c` are NXP's TFA98xx driver core
(Apache License 2.0, Copyright NXP Semiconductors; the Redmi K20 Pro's copy also carries Xiaomi's changes), taken from
`techpack/audio/asoc/codecs/tfa98xx` of https://github.com/crdroidandroid/android_kernel_xiaomi_sm8150 (branch 14.0,
commit 6405fa7bffb7e50d99d1d44dd30eccc5c9bde87d), the kernel the phone's Android uses.

Changes: every `#include <linux/...>` became `#include "kshim.h"`, and each `.c` includes `../inc/kshim.h` first.
`inc/kshim.h` (what the code wants of a Linux kernel), `inc/tfa_glue.h` and `src/tfa_glue.c` (the register and
raw-bus functions Linux's tfa98xx.c supplies) are new. The bus and the container loading are `bsd/dev/raphael_tfa.c`
and `bsd/dev/raphael_amp.c`.
