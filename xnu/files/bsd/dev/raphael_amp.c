/*
 * raphael: read-only probe of the NXP TFA98xx smart amplifier (I2C 0x34 on QUPv3 SE1,
 * reset GPIO 59) and bring-up of the quaternary MI2S pins. Boot-arg amp_probe=1.
 * I2C engine copied from raphael_touch.c. No amplifier registers are written.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
extern void hakios_tl(const char *, ...) __printflike(1, 2);

#define AMP_GPIO_CFG(n) ((uint32_t)(n) * 0x1000u)
#define AMP_GPIO_IO(n) (AMP_GPIO_CFG(n) + 4u)
#define AMP_MAX 10u
#define NORMANDY_PID 0x4535u
#define NORMANDY_EVENT 0x4100u
#define NORMANDY_ESD 0x30f3u
#define SE_STATUS       0x040u
#define SE_MCLK         0x048u
#define SE_FW           0x068u
#define SE_CLK_SEL      0x07cu
#define SE_DMA          0x258u
#define SE_TX_LEN       0x26cu
#define SE_RX_LEN       0x270u
#define SE_SCL          0x278u
#define SE_CMD          0x600u
#define SE_CTRL         0x604u
#define SE_IRQ          0x610u
#define SE_IRQ_EN       0x614u
#define SE_IRQ_CLR      0x618u
#define SE_TX           0x700u
#define SE_RX           0x780u
#define SE_TX_COUNT     0x800u
#define SE_RX_COUNT     0x804u
#define SE_HW_TX        0xe24u
#define SE_HW_RX        0xe28u
#define SE_TX_WM        0x80cu
#define SE_RX_WM_IRQ    (1u << 26)
#define SE_RX_LAST      (1u << 27)
#define SE_TX_WM_IRQ    (1u << 30)
#define SE_DONE         (1u << 0)
#define SE_NACK         (1u << 10)
#define SE_ERRORS       (0x3eu | (1u << 9) | (1u << 10) | (1u << 12) | \
                         (1u << 13) | (3u << 24) | (3u << 28))
#define SE_EVENTS       (SE_DONE | SE_ERRORS | SE_RX_WM_IRQ | SE_RX_LAST | SE_TX_WM_IRQ)
/* geni_se_config_packing(8, 4, true, true, true): start bits 7/15/23/31,
 * MSB-to-LSB, length 8, STOP only on vector 3. FIFO bytes remain LSB first. */
#define SE_PACK0        0x0007f8feu
#define SE_PACK1        0x000ffefeu

static struct {
	vm_offset_t se, gcc, gpio;
	bool started, dead, probing, invert;
	uint32_t irq_status, irq_last, active;
	uint32_t latency;
	uint64_t poll_time;
	unsigned sent, got;
	int x[AMP_MAX], y[AMP_MAX];
} st;
static int amp_sleep_chan;

static uint32_t
amp_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
amp_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
amp_wait_until(uint64_t deadline)
{
	(void)assert_wait_deadline(&amp_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
amp_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	amp_wait_until(deadline);
}

static bool
amp_wait_clear(vm_offset_t base, uint32_t off, uint32_t mask)
{
	for (unsigned i = 0; i < 5000; ++i) {
		if (!(amp_rd(base, off) & mask)) {
			return true;
		}
		delay(1);
	}
	return false;
}

static void
amp_gpio(unsigned pin, unsigned mux, unsigned pull, unsigned drive, bool out, bool high)
{
	uint32_t off = AMP_GPIO_CFG(pin);
	uint32_t cfg = amp_rd(st.gpio, off);
	if (out) {
		/* Set the desired latch before enabling output (reset is active low). */
		amp_wr(st.gpio, AMP_GPIO_IO(pin),
		    (amp_rd(st.gpio, AMP_GPIO_IO(pin)) & ~2u) | (high ? 2u : 0u));
	}
	cfg &= ~0x3ffu;
	cfg |= (mux << 2) | pull | (drive << 6) | (out ? 0x200u : 0u);
	amp_wr(st.gpio, off, cfg);
}

static void
amp_abort(void)
{
	amp_wr(st.se, SE_CTRL, 2); /* abort M command, release held bus */
	bool done = false;
	for (unsigned i = 0; i < 5000; ++i) {
		if (amp_rd(st.se, SE_IRQ) & (1u << 5)) {
			done = true;
			break;
		}
		delay(1);
	}
	if (!done || !amp_wait_clear(st.se, SE_STATUS, 1u)) {
		st.dead = true;
		printf("raphael-amp: abort failed status=%#x irq=%#x; stopping\n",
		    amp_rd(st.se, SE_STATUS), amp_rd(st.se, SE_IRQ));
	}
	/* Bound drain even if the hardware reports an impossible FIFO count. */
	for (unsigned i = 0; i < 64 && (amp_rd(st.se, SE_RX_COUNT) & 0x1ffffffu); ++i) {
		(void)amp_rd(st.se, SE_RX);
	}
	amp_wr(st.se, SE_IRQ_CLR, 0xffffffffu);
	if (amp_rd(st.se, SE_TX_COUNT) & 0xfffffffu) {
		st.dead = true;
		printf("raphael-amp: TX FIFO not empty after abort; stopping\n");
	}
}
static bool
amp_idle_fifo(void)
{
	/* A previous kernel can stop between RX arrival and its drain; never discard an active transfer. */
	if (amp_rd(st.se, SE_STATUS) & 0x1001u) return false;
	unsigned drained = 0;
	while (drained < 64 && (amp_rd(st.se, SE_RX_COUNT) & 0x1ffffffu)) {
		(void)amp_rd(st.se, SE_RX);
		drained++;
	}
	if (drained) printf("raphael-amp: drained %u stale RX FIFO words\n", drained);
	amp_wr(st.se, SE_IRQ_CLR, 0xffffffffu);
	return !(amp_rd(st.se, SE_TX_COUNT) & 0xfffffffu) &&
	       !(amp_rd(st.se, SE_RX_COUNT) & 0x1ffffffu);
}
static uint32_t
amp_irq(void)
{
	st.irq_last = amp_rd(st.se, SE_IRQ);
	st.irq_status |= st.irq_last;
	return st.irq_last;
}

static int
amp_msg_run(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx,
    unsigned nr, bool stretch)
{
	if (st.dead || nt > 600 || nr > 600 || (nt && nr) || (!nt && !nr)) {
		return EIO;
	}
	if (!amp_wait_clear(st.se, SE_STATUS, 1u)) {
		st.dead = true; /* no other serial-engine owner is supported */
		return EBUSY;
	}
	amp_wr(st.se, SE_IRQ_CLR, 0xffffffffu);
	amp_wr(st.se, SE_TX_WM, nt ? 1u : 0u);
	amp_wr(st.se, nr ? SE_RX_LEN : SE_TX_LEN, nr ? nr : nt);
	amp_wr(st.se, SE_CMD, ((nr ? 2u : 1u) << 27) |
	    ((uint32_t)addr << 9) | (stretch ? 4u : 0u));
	int err = ETIMEDOUT;
	for (unsigned i = 0; i < 20000; ++i) {
		uint32_t irq = amp_irq();
		if (irq & SE_ERRORS) {
			err = (irq & SE_NACK) ? ENXIO : EIO;
			/* A completed NACK is normal during address probing. Aborting an
			 * idle sequencer may never produce ABORT_DONE. Wait for completion
			 * and leave a clean idle engine available for the other IC. */
			for (unsigned j = 0; j < 5000 && !(irq & SE_DONE); ++j) {
				delay(1);
				irq = amp_irq();
			}
			if ((irq & SE_DONE) && !(amp_rd(st.se, SE_STATUS) & 1u) &&
			    !(amp_rd(st.se, SE_TX_COUNT) & 0xfffffffu) &&
			    !(amp_rd(st.se, SE_RX_COUNT) & 0x1ffffffu)) {
				amp_wr(st.se, SE_IRQ_CLR, irq);
				return err;
			}
			break;
		}
		if (nt && st.sent < nt && (irq & SE_TX_WM_IRQ)) {
			/* fill the FIFO (16 words of 4 bytes) as far as it has room; the last word ends the watermark */
			unsigned room = 16u - (amp_rd(st.se, SE_TX_COUNT) & 0xfffffffu);
			while (room-- && st.sent < nt) {
				uint32_t word = 0;
				for (unsigned j = 0; j < 4 && st.sent < nt; ++j) {
					word |= (uint32_t)tx[st.sent++] << (8u * j);
				}
				amp_wr(st.se, SE_TX, word);
			}
			if (st.sent >= nt) {
				amp_wr(st.se, SE_TX_WM, 0);
			}
			amp_wr(st.se, SE_IRQ_CLR, SE_TX_WM_IRQ);
		}
		uint32_t count = amp_rd(st.se, SE_RX_COUNT) & 0x1ffffffu;
		if (count > 64 || count > (nr - st.got + 3u) / 4u) {
			err = EIO;
			break;
		}
		while (count--) {
			uint32_t word = amp_rd(st.se, SE_RX);
			for (unsigned j = 0; j < 4 && st.got < nr; ++j) {
				rx[st.got++] = (uint8_t)(word >> (8u * j));
			}
		}
		/* DONE is checked after draining RX, so final short words aren't lost. */
		if (irq & SE_DONE) {
			/* Re-sample after the final drain; a late error must beat DONE. */
			irq = amp_irq();
			if (irq & SE_ERRORS) {
				continue;
			}
			amp_wr(st.se, SE_IRQ_CLR, irq);
			if (st.got == nr && st.sent == nt) {
				return 0;
			}
			err = EIO;
			break;
		}
		/* Preserve DONE/errors if they arrive during this iteration. */
		amp_wr(st.se, SE_IRQ_CLR, irq & (SE_RX_WM_IRQ | SE_RX_LAST | SE_TX_WM_IRQ));
		delay(1);
	}
	amp_abort();
	return err;
}

static int
amp_msg(uint8_t addr, const uint8_t *tx, unsigned nt, uint8_t *rx,
    unsigned nr, bool stretch)
{
	st.irq_status = st.irq_last = 0;
	st.sent = st.got = 0;
	int err = amp_msg_run(addr, tx, nt, rx, nr, stretch);
	if (st.probing) {
		printf("raphael-amp: probe xfer addr=%#x %s len=%u stretch=%u err=%d irq=%#x seen=%#x tx=%u rx=%u status=%#x\n",
		    addr, nr ? "read" : "write", nr ? nr : nt, stretch ? 1u : 0u,
		    err, st.irq_last, st.irq_status, st.sent, st.got,
		    amp_rd(st.se, SE_STATUS));
	}
	return err;
}

static int
amp_read(uint8_t addr, uint16_t reg, bool wide, uint8_t *buf, unsigned len)
{
	uint8_t ptr[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
	int err = amp_msg(addr, wide ? ptr : ptr + 1, wide ? 2u : 1u,
	    NULL, 0, true);
	if (err) {
		return err;
	}
	/* STOP_STRETCH on the pointer write retains the bus for a repeated START. */
	return amp_msg(addr, NULL, 0, buf, len, false);
}
static bool
amp_se_init(void)
{
	uint32_t fw = amp_rd(st.se, SE_FW);
	uint32_t tx = amp_rd(st.se, SE_HW_TX), rx = amp_rd(st.se, SE_HW_RX);
	printf("raphael-amp: se%u fw=%#x proto=%u status=%#x tx=%#x rx=%#x ios=%#x\n",
	    1u, fw, (fw >> 8) & 0xffu, amp_rd(st.se, SE_STATUS), tx, rx,
	    amp_rd(st.se, 0x908));
	if (((fw >> 8) & 0xffu) != 3u || (amp_rd(st.se, 0x64) & 1u) ||
	    ((tx >> 24) & 0x3fu) != 32u || ((rx >> 24) & 0x3fu) != 32u ||
	    ((tx >> 16) & 0x3fu) < 2u || ((rx >> 16) & 0x3fu) < 2u) {
		printf("raphael-amp: need ABL-loaded I2C firmware and 32-bit FIFOs; probes skipped\n");
		return false;
	}
	if (amp_rd(st.se, SE_STATUS) & 0x1001u) {
		printf("raphael-amp: serial engine already busy; probes skipped\n");
		return false;
	}
	amp_wr(st.se, 0xe18, 0); /* no GSI events */
	amp_wr(st.se, 0xe1c, 0); /* mask external IRQ routing; poll local status */
	amp_wr(st.se, 0x644, 0);
	amp_wr(st.se, SE_IRQ_CLR, 0xffffffffu);
	amp_wr(st.se, 0x648, 0xffffffffu);
	amp_wr(st.se, SE_DMA, 0);
	amp_wr(st.se, 0x28, amp_rd(st.se, 0x28) | 0x7fu);
	amp_wr(st.se, 0xe30, amp_rd(st.se, 0xe30) | 0x0fu);
	amp_wr(st.se, 0x24, 0x7f);
	amp_wr(st.se, 0x20, 1);
	amp_wr(st.se, SE_CLK_SEL, 0);
	amp_wr(st.se, SE_MCLK, (7u << 4) | 1u);
	amp_wr(st.se, SE_SCL, (10u << 20) | (11u << 10) | 26u); /* 100 kHz */
	/* Linux 8-bit I2C packing: MSB on the wire, LSB byte first in FIFO. */
	amp_wr(st.se, 0x254, 0);
	amp_wr(st.se, 0x260, SE_PACK0);
	amp_wr(st.se, 0x264, SE_PACK1);
	amp_wr(st.se, 0x284, SE_PACK0);
	amp_wr(st.se, 0x288, SE_PACK1);
	amp_wr(st.se, SE_TX_WM, 0);
	amp_wr(st.se, 0x810, 0);
	amp_wr(st.se, 0x814, ((rx >> 16) & 0x3fu) - 1u);
	amp_wr(st.se, SE_IRQ_EN, SE_EVENTS);
	printf("raphael-amp: packing tx=%#x/%#x rx=%#x/%#x gran=%#x\n",
	    amp_rd(st.se, 0x260), amp_rd(st.se, 0x264),
	    amp_rd(st.se, 0x284), amp_rd(st.se, 0x288), amp_rd(st.se, 0x254));
	if (amp_rd(st.se, 0x260) != SE_PACK0 || amp_rd(st.se, 0x264) != SE_PACK1 ||
	    amp_rd(st.se, 0x284) != SE_PACK0 || amp_rd(st.se, 0x288) != SE_PACK1 ||
	    amp_rd(st.se, 0x254) != 0) {
		printf("raphael-amp: packing readback mismatch; probes skipped\n");
		return false;
	}
	if (!amp_idle_fifo()) {
		printf("raphael-amp: FIFO could not be cleared while idle; probes skipped\n");
		return false;
	}
	return true;
}


/* SM8150 GCC, QUPv3 wrapper 0, serial engine 1 (clock list from mainline gcc-sm8150.c). */
static bool
amp_clocks(void)
{
	uint32_t vote = 0x5200cu;
	uint32_t mask = (1u << 6) | (1u << 7) | (1u << 11);
	uint32_t rcg = 0x17278u;
	amp_wr(st.gcc, vote, amp_rd(st.gcc, vote) | mask);
	uint32_t dfs = amp_rd(st.gcc, rcg + 0x14u);
	uint32_t cfg = amp_rd(st.gcc, rcg + 4u);
	printf("raphael-amp: gcc vote=%#x rcg=%#x cfg=%#x dfs=%#x\n",
	    amp_rd(st.gcc, vote), amp_rd(st.gcc, rcg), cfg, dfs);
	if (dfs & 1u) {
		cfg = amp_rd(st.gcc, rcg + 0x1cu);
		if ((cfg & 0x371fu) != 1u) {
			printf("raphael-amp: DFS level0=%#x is not XO/1; need clock integration\n", cfg);
			return false;
		}
	} else {
		cfg = (cfg & ~(0x371fu | (1u << 20))) | 1u;
		amp_wr(st.gcc, rcg + 4u, cfg);
		amp_wr(st.gcc, rcg, amp_rd(st.gcc, rcg) | 1u);
		if (!amp_wait_clear(st.gcc, rcg, 1u)) {
			printf("raphael-amp: RCG update timeout\n");
			return false;
		}
	}
	const uint32_t branch[] = {0x17004, 0x17008, 0x17274};
	for (unsigned i = 0; i < 3; ++i) {
		if (branch[i] == 0x17008 && (amp_rd(st.gcc, branch[i]) & 2u)) {
			printf("raphael-amp: S-AHB hardware clock gating active\n");
			continue;
		}
		if (!amp_wait_clear(st.gcc, branch[i], 1u << 31)) {
			printf("raphael-amp: GCC branch %#x halted (%#x)\n", branch[i],
			    amp_rd(st.gcc, branch[i]));
			return false;
		}
	}
	return true;
}

/* TFA98xx register read: write the register number, read two bytes (MSB first). */
static int
amp_reg(uint8_t reg, uint16_t *val)
{
	uint8_t b[2] = {0, 0};
	int err = amp_read(0x34, reg, false, b, 2);
	*val = (uint16_t)(b[0] << 8 | b[1]);
	return err;
}

static void
amp_ids(const char *when)
{
	uint16_t r00 = 0, r01 = 0, r03 = 0;
	int e0 = amp_reg(0x00, &r00), e1 = amp_reg(0x01, &r01), e3 = amp_reg(0x03, &r03);
	printf("raphael-amp: %s: reg0=%#06x(%d) reg1=%#06x(%d) id(reg3)=%#06x(%d)\n",
	    when, r00, e0, r01, e1, r03, e3);
}

/* ---- what the amplifier driver (raphael_tfa.c) uses ------------------------------------------------- */

static bool opened;

/* Map, clock and set up the serial engine and its two pins, once. */
bool
raphael_amp_open(void)
{
	if (opened) {
		return !st.dead;
	}
	st.gcc = ml_io_map(0x00100000u, 0x60000);
	st.gpio = ml_io_map(0x03d00000u, 0x90000);
	st.se = ml_io_map(0x00884000u, 0x4000);
	if (!st.gcc || !st.gpio || !st.se || !amp_clocks() || !amp_se_init()) {
		printf("raphael-amp: cannot open the bus\n");
		st.dead = true;
		return false;
	}
	amp_gpio(114, 1, 0, 0, false, false);
	amp_gpio(115, 1, 0, 0, false, false);
	opened = true;
	return true;
}

/* The amplifier's reset line (GPIO 59): the NXP driver pulses it high, then low. */
void
raphael_amp_reset_pulse(void)
{
	amp_gpio(59, 0, 0, 0, true, true);
	amp_wait(5);
	amp_gpio(59, 0, 0, 0, true, false);
	amp_wait(20);
}

void
raphael_amp_sleep(unsigned ms)
{
	amp_wait(ms);
}

int
raphael_amp_write(uint8_t addr, const uint8_t *data, unsigned len)
{
	st.irq_status = st.irq_last = 0;
	st.sent = st.got = 0;
	return amp_msg_run(addr, data, len, NULL, 0, false);
}

int
raphael_amp_read(uint8_t addr, uint8_t reg, uint8_t *data, unsigned len)
{
	int err = amp_msg(addr, &reg, 1, NULL, 0, true);

	if (err) {
		return err;
	}
	return amp_msg(addr, NULL, 0, data, len, false);
}

static void
amp_thread(void *arg __unused, wait_result_t result __unused)
{
	amp_wait(20000); /* the kernel log only keeps what is printed after user space starts */
	st.gcc = ml_io_map(0x00100000u, 0x60000);
	st.gpio = ml_io_map(0x03d00000u, 0x90000);
	st.se = ml_io_map(0x00884000u, 0x4000);
	if (!st.gcc || !st.gpio || !st.se) {
		printf("raphael-amp: mapping failed\n");
		goto out;
	}
	printf("raphael-amp: gpio59=%#x gpio114=%#x gpio115=%#x gpio137=%#x gpio139=%#x gpio141=%#x\n",
	    amp_rd(st.gpio, AMP_GPIO_CFG(59)), amp_rd(st.gpio, AMP_GPIO_CFG(114)),
	    amp_rd(st.gpio, AMP_GPIO_CFG(115)), amp_rd(st.gpio, AMP_GPIO_CFG(137)),
	    amp_rd(st.gpio, AMP_GPIO_CFG(139)), amp_rd(st.gpio, AMP_GPIO_CFG(141)));
	if (!amp_clocks() || !amp_se_init()) {
		printf("raphael-amp: initialization failed\n");
		goto out;
	}
	/* I2C pins: qup1 (mux 1), no pull, lowest drive, as the device tree says. */
	amp_gpio(114, 1, 0, 0, false, false);
	amp_gpio(115, 1, 0, 0, false, false);
	amp_wait(5);
	st.probing = true;
	amp_ids("as found");
	/* Reset line (GPIO 59): the NXP driver pulses it high then low; low is "running". */
	amp_gpio(59, 0, 0, 0, true, true);
	amp_wait(5);
	amp_gpio(59, 0, 0, 0, true, false);
	amp_wait(20);
	amp_ids("after reset pulse (high, then low)");
	/* Leave the amplifier idle: nothing was written to it. */
	st.probing = false;
	printf("raphael-amp: probe done\n");
out:
	st.dead = true;
	thread_terminate(current_thread());
}

void
raphael_amp_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (st.started || !hakios_is_raphael() ||
	    !PE_parse_boot_argn("amp_probe", &enabled, sizeof(enabled)) || !enabled) return;
	st.started = true;
	if (kernel_thread_start(amp_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("raphael-amp: cannot create thread\n");
	}
}
