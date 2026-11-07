#ifndef TFA_GLUE_H
#define TFA_GLUE_H
#include <stdbool.h>
#include "tfa_device.h"

struct tfa_bus {
	int (*write)(const uint8_t *data, unsigned length);                   /* 0 if it went */
	int (*read)(uint8_t reg, uint8_t *data, unsigned length);             /* pointer write, repeated start, read */
	int (*dsp_write)(const char *message, unsigned length);               /* may be NULL */
	int (*dsp_read)(uint8_t *message, unsigned length);                   /* may be NULL */
};

void tfa_glue_bus(const struct tfa_bus *bus);
struct tfa_device *tfa_glue_device(void);
bool tfa_glue_open(void *container, int length, int slave);
#endif
