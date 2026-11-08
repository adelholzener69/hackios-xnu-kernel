/*
 * Display brightness for the phones whose panel has no backlight service iOS can use.
 *
 * iOS's brightness slider ends in backboardd's -[BKDisplayBrightnessController
 * setBrightnessLevel:reason:options:]; the user-space hook (userland/src/inject.c, boot-arg
 * brightness=1) calls the original and then writes the level here, in permille:
 *
 *   sysctl kern.hakios.brightness        0 .. 1000, 1000 = full (default)
 *   sysctl kern.hakios.display_dump=1    log the display mixer's registers (read only)
 *
 * What the level does is up to the display driver of the phone (raphael: bsd/dev/hakios.adreno.c).
 */
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern void raphael_display_set_brightness(uint32_t permille);
extern void raphael_display_dump_mixer(void);

SYSCTL_DECL(_kern_hakios);

static int brightness_permille = 1000;

static int
brightness_handler(struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = brightness_permille;
	int error = sysctl_handle_int(oidp, &value, 0, req);

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (value < 0) {
		value = 0;
	} else if (value > 1000) {
		value = 1000;
	}
	brightness_permille = value;
	printf("hakios: brightness %d/1000\n", value);
	raphael_display_set_brightness((uint32_t)value);
	return 0;
}

static int
display_dump_handler(struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = 0;
	int error = sysctl_handle_int(oidp, &value, 0, req);

	if (error == 0 && req->newptr != USER_ADDR_NULL && value != 0) {
		raphael_display_dump_mixer();
	}
	return error;
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, brightness, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, NULL, 0,
    brightness_handler, "I", "display brightness in permille (0..1000)");
SYSCTL_PROC(_kern_hakios, OID_AUTO, display_dump, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, NULL, 0,
    display_dump_handler, "I", "write 1 to log the display mixer registers");
