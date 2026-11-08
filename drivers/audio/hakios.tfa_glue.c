/*
 * The few functions the NXP TFA98xx code expects of its host (the Linux driver's tfa98xx.c does them with
 * regmap and i2c_transfer), done on a bus given by the caller: raphael_tfa.c in this kernel, a fake chip in
 * the host test. This file is new (not NXP's) and builds on the host as it does in the kernel.
 */
#include "tfa98xx/inc/kshim.h"
#include "tfa98xx/inc/dbgprint.h"
#include "tfa98xx/inc/tfa_container.h"
#include "tfa98xx/inc/tfa.h"
#include "tfa98xx/inc/tfa_glue.h"

static struct tfa_bus bus;
static struct tfa_device device;

void
tfa_glue_bus(const struct tfa_bus *b)
{
	bus = *b;
}

struct tfa_device *
tfa_glue_device(void)
{
	return &device;
}

enum Tfa98xx_Error
tfa98xx_write_register16(struct tfa_device *tfa, unsigned char subaddress, unsigned short value)
{
	uint8_t b[3] = { subaddress, (uint8_t)(value >> 8), (uint8_t)value };

	(void)tfa;
	return bus.write(b, 3) ? Tfa98xx_Error_Fail : Tfa98xx_Error_Ok;
}

enum Tfa98xx_Error
tfa98xx_read_register16(struct tfa_device *tfa, unsigned char subaddress, unsigned short *val)
{
	uint8_t b[2];

	(void)tfa;
	if (bus.read(subaddress, b, 2)) {
		return Tfa98xx_Error_Fail;
	}
	*val = (unsigned short)(b[0] << 8 | b[1]);
	return Tfa98xx_Error_Ok;
}

enum Tfa98xx_Error
tfa98xx_write_raw(struct tfa_device *tfa, int len, const unsigned char data[])
{
	(void)tfa;
	return bus.write(data, (unsigned)len) ? Tfa98xx_Error_Fail : Tfa98xx_Error_Ok;
}

enum Tfa98xx_Error
tfa98xx_read_data(struct tfa_device *tfa, unsigned char reg, int len, unsigned char value[])
{
	(void)tfa;
	return bus.read(reg, value, (unsigned)len) ? Tfa98xx_Error_Fail : Tfa98xx_Error_Ok;
}

/*
 * The TFA9874 has no DSP of its own: the speaker-protection algorithm runs on the SoC's audio DSP, and
 * the driver's messages for it (has_msg = 1) go there. Here they go to the bus' hook, if any, and are
 * otherwise only counted: the amplifier then runs without that protection (so keep it quiet).
 */
enum Tfa98xx_Error
tfa98xx_init_dsp(struct tfa_device *tfa)
{
	(void)tfa;
	return Tfa98xx_Error_Not_Supported;
}

enum Tfa98xx_Error
tfa98xx_write_dsp(struct tfa_device *tfa, int num_bytes, const char *command_buffer)
{
	(void)tfa;
	if (bus.dsp_write) {
		return bus.dsp_write(command_buffer, (unsigned)num_bytes) ? Tfa98xx_Error_Fail : Tfa98xx_Error_Ok;
	}
	return Tfa98xx_Error_Ok;
}

enum Tfa98xx_Error
tfa98xx_read_dsp(struct tfa_device *tfa, int num_bytes, unsigned char *result_buffer)
{
	(void)tfa;
	memset(result_buffer, 0, (size_t)num_bytes);
	if (bus.dsp_read) {
		return bus.dsp_read(result_buffer, (unsigned)num_bytes) ? Tfa98xx_Error_Fail : Tfa98xx_Error_Ok;
	}
	return Tfa98xx_Error_Ok;
}

/* Load the container and find the device in it; false (with the reason said) if anything is missing. */
bool
tfa_glue_open(void *container, int length, int slave)
{
	struct tfa_device *tfa = &device;

	memset(tfa, 0, sizeof(*tfa));
	if (tfa_load_cnt(container, length) != tfa_error_ok) {
		printf("tfa: the container does not load\n");
		return false;
	}
	tfa->cnt = container;
	tfa->buffer_size = 65536;
	tfa->cachep = (struct kmem_cache *)&device;      /* (the allocator ignores which) */
	if (tfa_dev_probe(slave, tfa) != 0) {
		printf("tfa: no amplifier at %#x\n", slave);
		return false;
	}
	tfa->has_msg = tfa->is_probus_device ? 1 : 0;
	tfa->dev_idx = tfa_cont_get_idx(tfa);
	if (tfa->dev_idx < 0) {
		printf("tfa: the amplifier at %#x is not in the container\n", slave);
		return false;
	}
	return true;
}

char *
tfa_strstr(const char *haystack, const char *needle)
{
	size_t n = strlen(needle);

	if (n == 0) {
		return (char *)(uintptr_t)haystack;
	}
	for (; *haystack != 0; haystack++) {
		if (*haystack == *needle && memcmp(haystack, needle, n) == 0) {
			return (char *)(uintptr_t)haystack;
		}
	}
	return NULL;
}
