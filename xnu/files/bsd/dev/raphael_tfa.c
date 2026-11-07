/*
 * raphael: the speaker amplifier (NXP TFA9874 at I2C 0x34) started from the phone's own container
 * (mono.cnt, in /fw), with the NXP code Xiaomi's kernel for this phone uses (bsd/dev/tfa98xx/, Apache-2.0)
 * on the I2C bus of raphael_amp.c. Called from raphael_mi2s.c once the audio DSP is running the interface's
 * clock (a TFA9874 locks to the bit clock). Boot-arg tfa=1 asks for it (with adsp_step=12).
 */
#include <kern/kalloc.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/hakios.audio.h>
#include "tfa98xx/inc/kshim.h"
#include "tfa98xx/inc/tfa.h"
#include "tfa98xx/inc/tfa_container.h"
#include "tfa98xx/inc/tfa_glue.h"

extern void loader_log_putc(char c);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern bool raphael_amp_open(void);
extern void raphael_amp_reset_pulse(void);
extern void raphael_amp_sleep(unsigned ms);
extern int raphael_amp_write(uint8_t addr, const uint8_t *data, unsigned len);
extern int raphael_amp_read(uint8_t addr, uint8_t reg, uint8_t *data, unsigned len);

#define TFA_ADDR 0x34

/* ---- what the NXP code asks of its kernel ------------------------------------------------------------ */

#define TFA_CACHE_BYTES 4096

void *
kmem_cache_alloc(struct kmem_cache *cache, int flags)
{
	(void)cache;
	(void)flags;
	return kalloc_data(TFA_CACHE_BYTES, Z_WAITOK | Z_ZERO);
}

void
kmem_cache_free(struct kmem_cache *cache, void *object)
{
	(void)cache;
	kfree_data(object, TFA_CACHE_BYTES);
}

void *
tfa_kmalloc(size_t size)
{
	uint64_t *p = kalloc_data(size + 16, Z_WAITOK | Z_ZERO);

	if (p == NULL) {
		return NULL;
	}
	p[0] = size + 16;
	return p + 2;
}

void
tfa_kfree(void *pointer)
{
	uint64_t *p = (uint64_t *)pointer;

	if (p != NULL) {
		uint64_t *base = p - 2;

		kfree_data(base, base[0]);
	}
}

void
msleep_interruptible(unsigned ms)
{
	raphael_amp_sleep(ms);
}

/* Linux's crc32_le: reflected, polynomial 0xedb88320, no final inversion. */
uint32_t
crc32_le(uint32_t crc, const unsigned char *data, size_t length)
{
	while (length--) {
		crc ^= *data++;
		for (int k = 0; k < 8; k++) {
			crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1u));
		}
	}
	return crc;
}

/* ---- the bus, and what is said ---------------------------------------------------------------------- */

static int
bus_write(const uint8_t *data, unsigned length)
{
	return raphael_amp_write(TFA_ADDR, data, length);
}

static int
bus_read(uint8_t reg, uint8_t *data, unsigned length)
{
	return raphael_amp_read(TFA_ADDR, reg, data, length);
}

static unsigned dsp_messages;

extern int raphael_tfadsp_write(const char *message, unsigned length);
extern int raphael_tfadsp_read(uint8_t *result, unsigned length);

static int
dsp_write(const char *message, unsigned length)
{
	dsp_messages++;
	return raphael_tfadsp_write(message, length);
}

static int
dsp_read(uint8_t *result, unsigned length)
{
	return raphael_tfadsp_read(result, length);
}

static uint32_t tfa_debug;       /* boot-arg tfa_debug=1: the amplifier's registers are dumped */
static const uint8_t *container;
static uint64_t container_size;
static bool open_ok;

/*
 * Start the amplifier with profile 0, vstep 0, 16-bit samples. Returns true if the NXP code says it
 * is started. `say' gets the lines worth showing.
 */
void raphael_tfa_dump(const char *when, void (*say)(const char *));

bool
raphael_tfa_start(void (*say)(const char *))
{
	struct tfa_bus b = { bus_write, bus_read, dsp_write, dsp_read };
	struct tfa_device *tfa;
	char line[200];
	int err;

	PE_parse_boot_argn("tfa_debug", &tfa_debug, sizeof(tfa_debug));
	if (!raphael_amp_open()) {
		say("tfa: the I2C bus does not open\n");
		return false;
	}
	tfa_glue_bus(&b);
	{
		/* the small image first (ADSP=1), else the root; a copy of our own, which the NXP code may write to */
		uint64_t size = 0;
		const uint8_t *file = hakios_image_file("fw", "mono.cnt", &size, NULL);
		bool from_root = false;

		if (file == NULL && (file = hakios_adsp_fw_file("mono.cnt", &size)) != NULL) {
			from_root = true;
		}
		if (file != NULL) {
			uint8_t *copy = kalloc_data(size, Z_WAITOK | Z_ZERO);

			if (copy != NULL) {
				memcpy(copy, file, size);
				container = copy;
				container_size = size;
			}
			if (from_root) {
				hakios_adsp_fw_free(file, size);
			}
		}
	}
	if (container == NULL) {
		say("tfa: no /fw/mono.cnt in the image (copy it to fw-audio/ and build with ADSP=1)\n");
		return false;
	}
	snprintf(line, sizeof(line), "tfa: container mono.cnt, %llu bytes\n", container_size);
	say(line);
	raphael_amp_reset_pulse();
	open_ok = tfa_glue_open((void *)(uintptr_t)container, (int)container_size, TFA_ADDR);
	tfa = tfa_glue_device();
	if (!open_ok) {
		say("tfa: the container or the amplifier is not what was expected\n");
		return false;
	}
	snprintf(line, sizeof(line), "tfa: amplifier rev %#x, family %d, probus %d, %d profiles, first \"%s\"\n", tfa->rev,
	    tfa->tfa_family, tfa->is_probus_device, tfa_cnt_get_dev_nprof(tfa), tfaContProfileName(tfa->cnt, tfa->dev_idx, 0));
	say(line);
	if (tfa_is_cold(tfa) == 0) {
		say("tfa: the amplifier is still warm: reset it\n");
		tfa_reset(tfa);
	}
	err = tfa_dev_start(tfa, 0, 0, 24);      /* Android's speaker is 24 bits in 32-bit slots (audio_platform_info.xml) */
	raphael_tfa_dump("right after tfa_dev_start", say);
	if (err == 0) {
		/* (Android's driver un-mutes after every start) */
		int mute_err = tfa_dev_set_state(tfa, TFA_STATE_UNMUTE, 0);

		snprintf(line, sizeof(line), "tfa: un-muted (%d)\n", mute_err);
		say(line);
		/* boot-arg tfa_debug=1: its state over the next two seconds: control 0, status 0x10, 0x11, manager 0x14 */
		static const unsigned waits[] = { 5, 20, 75, 150, 250, 500, 1000 };
		unsigned t = 0;

		raphael_tfa_dump("just after the un-mute", say);
		for (unsigned i = 0; tfa_debug && i < sizeof(waits) / sizeof(waits[0]); i++) {
			uint8_t r0[2] = { 0, 0 }, r10[2] = { 0, 0 }, r11[2] = { 0, 0 }, r14[2] = { 0, 0 };

			raphael_amp_sleep(waits[i]);
			t += waits[i];
			bus_read(0x00, r0, 2);
			bus_read(0x10, r10, 2);
			bus_read(0x11, r11, 2);
			bus_read(0x14, r14, 2);
			snprintf(line, sizeof(line), "tfa: +%u ms: 0=%02x%02x 10=%02x%02x 11=%02x%02x 14=%02x%02x\n", t, r0[0], r0[1], r10[0],
			    r10[1], r11[0], r11[1], r14[0], r14[1]);
			say(line);
		}
	}
	snprintf(line, sizeof(line), "tfa: tfa_dev_start(profile 0) returned %d; %u messages went to the audio DSP's TFADSP module\n",
	    err, dsp_messages);
	say(line);
	return err == 0;
}

void
raphael_tfa_stop(void (*say)(const char *))
{
	if (open_ok) {
		int err = tfa_dev_stop(tfa_glue_device());
		char line[80];

		snprintf(line, sizeof(line), "tfa: stopped (%d)\n", err);
		say(line);
	}
	if (container != NULL) {
		uint8_t *ours = __DECONST(uint8_t *, container);

		kfree_data(ours, container_size);
		container = NULL;
	}
}

/* The amplifier's registers 0x00 to 0x0f (16 bits each), for a look at what it thinks it is doing. */
void
raphael_tfa_dump(const char *when, void (*say)(const char *))
{
	char line[260];
	int n = 0;

	if (!tfa_debug) {
		return;
	}
	n += snprintf(line + n, sizeof(line) - n, "tfa: registers %s:", when);
	for (unsigned reg = 0; reg < 0x18; reg++) {
		if (reg == 7 || reg == 10 || reg == 11 || reg == 12 || reg == 13 || reg == 14 || reg == 15) {
			continue;
		}
		uint8_t b[2] = { 0, 0 };

		if (bus_read((uint8_t)reg, b, 2)) {
			n += snprintf(line + n, sizeof(line) - n, " %x=??", reg);
		} else {
			n += snprintf(line + n, sizeof(line) - n, " %x=%02x%02x", reg, b[0], b[1]);
		}
	}
	snprintf(line + n, sizeof(line) - n, "\n");
	say(line);
}

/* On a panic's way out (one processor, no lock, no sleep): the amplifier into power-down (PWDN, register 0 bit 0). */
extern void raphael_amp_panic_write(uint8_t addr, const uint8_t *data, unsigned len);

void
raphael_tfa_quiesce(void)
{
	const uint8_t powerdown[3] = { 0x00, 0x00, 0x01 };

	raphael_amp_panic_write(TFA_ADDR, powerdown, sizeof(powerdown));
}
