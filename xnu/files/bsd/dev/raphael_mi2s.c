/*
 * raphael: sub-step (b) of the speaker path. The audio DSP is asked (APR, service AFE) for the
 * quaternary MI2S interface Android's speaker uses (QUAT_MI2S_RX, port 0x1006): its bit clock,
 * the interface's configuration (I2S, master, 48 kHz, 16 bits, two channels on data line 0), and a
 * start. The pins GPIO 137 to 140 (south TLMM tile) are switched to the interface function for it.
 * Whether it works is looked at on the pins: the bit clock (GPIO 137) and the word select (138)
 * are sampled for edges. Nothing is sent to the amplifier. Runs as step 12 of adsp_step=N
 * (hakios.adsp.c) on the Redmi K20 Pro only; boot-arg mi2s_hold=S sets how long it is held (3).
 *
 * Numbers: Linux sound/soc/qcom/qdsp6/q6afe.c (the message layouts, clock ids), the phone's own device tree
 * (pins: quat_mi2s_active, drive 8 mA, no pull, master; rx line 0, tx line 1).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <libkern/libkern.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);


/* said to the loader's log as well, as it happens: the kernel's own log may lose it */
static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
	return length;
}

#define M_AFE                   4
#define M_WAIT_MS               1000
#define M_PORT                  0x1006u
#define M_OP_START              0x000100e5u
#define M_OP_STOP               0x000100e6u
#define M_OP_SET_PARAM_V2       0x000100efu
#define M_OP_SVC_SET_PARAM      0x000100f3u
#define M_MOD_DEV_INTERFACE     0x0001020cu
#define M_PARAM_I2S_CONFIG      0x0001020du
#define M_MOD_CLOCK_SET         0x0001028fu
#define M_PARAM_CLOCK_SET       0x00010290u
#define M_CLK_QUAD_MI2S_IBIT    0x106u
#define M_CLK_COUPLE_NO         1u
#define M_RATE                  48000u
#define M_BITS                  16u
#define M_RESULT                0x000110e8u

struct m_param {
	uint32_t module, param;
	uint16_t size, reserved;
} __attribute__((packed));

struct m_clock {
	uint32_t version, id, hz;
	uint16_t attribute, root;
	uint32_t enable;
} __attribute__((packed));

struct m_i2s {
	uint32_t version;
	uint16_t bit_width, channel_mode, mono_stereo, ws_src;
	uint32_t sample_rate;
	uint16_t data_format, reserved;
} __attribute__((packed));

static vm_offset_t gpio;

static uint32_t
m_rd(uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(gpio + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
m_wr(uint32_t off, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(gpio + off) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

/* mux 0 = plain GPIO, 1 = quaternary MI2S; drive is in 2 mA steps minus one (3 = 8 mA); pull 0 none, 1 down */
static void
m_pin(unsigned pin, unsigned mux, unsigned pull, unsigned drive, bool out, bool high)
{
	uint32_t cfg = m_rd(pin * 0x1000u);

	if (out) {
		m_wr(pin * 0x1000u + 4u, (m_rd(pin * 0x1000u + 4u) & ~2u) | (high ? 2u : 0u));
	}
	cfg &= ~0x3ffu;
	cfg |= (mux << 2) | pull | (drive << 6) | (out ? 0x200u : 0u);
	m_wr(pin * 0x1000u, cfg);
}

static bool
m_cmd(const char *what, uint16_t port, uint32_t opcode, const void *payload, uint32_t length)
{
	struct hakios_apr_answer a;

	if (!hakios_adsp_apr(M_AFE, port, 0, opcode, payload, length, M_WAIT_MS, &a)) {
		say("mi2s: %s (opcode %#x): NO ANSWER\n", what, opcode);
		return false;
	}
	if (a.opcode == M_RESULT && a.words[0] == opcode) {
		say("mi2s: %s: %s (status %u)\n", what, a.words[1] == 0 ? "done" : "REFUSED", a.words[1]);
		return a.words[1] == 0;
	}
	say("mi2s: %s: unexpected answer %#x (%u bytes) %#x %#x\n", what, a.opcode, a.length, a.words[0], a.words[1]);
	return false;
}

static bool
m_param(const char *what, bool service, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	uint8_t packet[16 + sizeof(struct m_param) + 32];
	struct m_param *p = (struct m_param *)(packet + 16);

	bzero(packet, sizeof(packet));
	if (service) {
		*(uint32_t *)packet = (uint32_t)sizeof(*p) + size;      /* in band: no address, no handle */
	} else {
		*(uint16_t *)packet = M_PORT;
		*(uint16_t *)(packet + 2) = (uint16_t)(sizeof(*p) + size);
	}
	p->module = module;
	p->param = param;
	p->size = size;
	memcpy(packet + 16 + sizeof(*p), data, size);
	return m_cmd(what, 0, service ? M_OP_SVC_SET_PARAM : M_OP_SET_PARAM_V2, packet, 16 + (uint32_t)sizeof(*p) + size);
}

static void
m_edges(const char *when, unsigned secs)
{
	uint64_t end;
	unsigned i, e137 = 0, e138 = 0, e139 = 0, n = 0;
	uint32_t p137 = m_rd(137 * 0x1000u + 4u) & 1u, p138 = m_rd(138 * 0x1000u + 4u) & 1u, p139 = m_rd(139 * 0x1000u + 4u) & 1u;

	(void)secs;
	for (i = 0; i < 200000; i++, n++) {
		uint32_t a = m_rd(137 * 0x1000u + 4u) & 1u, b = m_rd(138 * 0x1000u + 4u) & 1u, c = m_rd(139 * 0x1000u + 4u) & 1u;

		e137 += a != p137;
		e138 += b != p138;
		e139 += c != p139;
		p137 = a;
		p138 = b;
		p139 = c;
	}
	(void)end;
	say("mi2s: %s: of %u samples the bit clock (137) changed %u times, word select (138) %u, data (139) %u\n", when, n, e137,
	    e138, e139);
}

/*
 * The speaker protection: the TFA9874 has no DSP of its own, its algorithm is the audio DSP's module TFADSP_RX
 * (0x1000B911) on the quaternary MI2S RX port, and the NXP driver's messages for it go there as the parameter
 * 0x1000B921 (set) and are answered through 0x1000B922 (get). In band, parameter header version 3 (module,
 * instance, parameter, size), as Xiaomi's q6afe.c does when the DSP knows instance ids.
 */
#define M_OP_SET_PARAM_V3       0x000100fau
#define M_OP_GET_PARAM_V3       0x000100fbu
#define M_RSP_GET_PARAM_V3      0x00010108u
#define M_MOD_TFADSP_RX         0x1000b911u
#define M_PARAM_TFADSP_RX_CFG   0x1000b921u
#define M_PARAM_TFADSP_RX_GET   0x1000b922u

struct m_param3 {
	uint32_t module;
	uint16_t instance, reserved;
	uint32_t param, size;
} __attribute__((packed));

static unsigned tfadsp_sent, tfadsp_refused, tfadsp_read;

int
raphael_tfadsp_write(const char *message, unsigned length)
{
	uint8_t packet[16 + sizeof(struct m_param3) + 4096];
	struct m_param3 *p = (struct m_param3 *)(packet + 16);
	unsigned padded = (length + 3u) & ~3u;
	struct hakios_apr_answer a;

	if (padded > 4096) {
		say("mi2s: a TFADSP message of %u bytes is too long\n", length);
		return 1;
	}
	bzero(packet, 16 + sizeof(*p) + padded);
	*(uint16_t *)packet = M_PORT;
	*(uint16_t *)(packet + 2) = (uint16_t)(sizeof(*p) + padded);
	p->module = M_MOD_TFADSP_RX;
	p->param = M_PARAM_TFADSP_RX_CFG;
	p->size = padded;
	memcpy(packet + 16 + sizeof(*p), message, length);
	tfadsp_sent++;
	if (!hakios_adsp_apr(M_AFE, M_PORT, 0, M_OP_SET_PARAM_V3, packet, 16 + (uint32_t)sizeof(*p) + padded, M_WAIT_MS, &a) ||
	    a.opcode != M_RESULT || a.words[1] != 0) {
		tfadsp_refused++;
		say("mi2s: TFADSP message %u (%u bytes, %02x %02x %02x): %s %#x\n", tfadsp_sent, length, (uint8_t)message[0],
		    (uint8_t)message[1], (uint8_t)message[2], a.opcode == M_RESULT ? "REFUSED, status" : "no answer, opcode", a.opcode == M_RESULT ? a.words[1] : a.opcode);
		return 1;
	}
	return 0;
}

int
raphael_tfadsp_read(uint8_t *result, unsigned length)
{
	uint8_t packet[16 + sizeof(struct m_param3)];
	struct m_param3 *p = (struct m_param3 *)(packet + 16);
	struct hakios_apr_answer a;

	bzero(packet, sizeof(packet));
	*(uint16_t *)packet = M_PORT;
	*(uint16_t *)(packet + 2) = (uint16_t)sizeof(*p);
	p->module = M_MOD_TFADSP_RX;
	p->param = M_PARAM_TFADSP_RX_GET;
	p->size = (length + 3u) & ~3u;
	tfadsp_read++;
	if (!hakios_adsp_apr(M_AFE, M_PORT, 0, M_OP_GET_PARAM_V3, packet, sizeof(packet), M_WAIT_MS, &a) || a.opcode != M_RSP_GET_PARAM_V3) {
		say("mi2s: TFADSP read %u: %s %#x\n", tfadsp_read, a.opcode == M_RESULT ? "refused, status" : "no answer, opcode",
		    a.opcode == M_RESULT ? a.words[1] : a.opcode);
		return 1;
	}
	/* the answer: status, the parameter header (16 bytes), the data */
	if (a.words[0] != 0) {
		say("mi2s: TFADSP read %u: status %u\n", tfadsp_read, a.words[0]);
		return 1;
	}
	memcpy(result, (const uint8_t *)a.words + 4 + sizeof(struct m_param3), length);
	return 0;
}

void
raphael_tfadsp_report(void)
{
	say("mi2s: TFADSP: %u messages sent, %u refused or unanswered, %u reads\n", tfadsp_sent, tfadsp_refused, tfadsp_read);
}

extern bool raphael_tfa_start(void (*say)(const char *));
extern void raphael_tfa_stop(void (*say)(const char *));

static void
say_line(const char *text)
{
	say("%s", text);
}

bool
raphael_mi2s_step(void)
{
	uint32_t hold = 3, tfa = 0;
	struct m_clock clk = { 1, M_CLK_QUAD_MI2S_IBIT, M_RATE * M_BITS * 2, M_CLK_COUPLE_NO, 0, 1 };
	struct m_i2s cfg = { .version = 1, .bit_width = M_BITS, .channel_mode = 1 /* SD0 */, .mono_stereo = 1 /* stereo */,
		             .ws_src = 1 /* internal: we are the master */, .sample_rate = M_RATE, .data_format = 0 };
	uint16_t port[2] = { M_PORT, 0 };
	bool ok;

	if (!hakios_is_raphael()) {
		return true;
	}
	PE_parse_boot_argn("mi2s_hold", &hold, sizeof(hold));
	gpio = ml_io_map(0x03d00000u, 0x90000);
	if (!gpio) {
		say("mi2s: cannot map the pins\n");
		return false;
	}
	say("mi2s: pins before: 137=%#x 138=%#x 139=%#x 140=%#x\n", m_rd(137 * 0x1000u), m_rd(138 * 0x1000u), m_rd(139 * 0x1000u),
	    m_rd(140 * 0x1000u));
	m_edges("before", 0);
	/* the clock and word select: output, high, 8 mA; the data line out, the feedback line in */
	m_pin(137, 1, 0, 3, true, true);
	m_pin(138, 1, 0, 3, true, true);
	m_pin(139, 1, 0, 3, false, false);
	m_pin(140, 1, 0, 3, false, false);
	say("mi2s: pins on the interface: 137=%#x 138=%#x 139=%#x 140=%#x\n", m_rd(137 * 0x1000u), m_rd(138 * 0x1000u),
	    m_rd(139 * 0x1000u), m_rd(140 * 0x1000u));
	ok = m_param("the quaternary MI2S bit clock (0x106) at 1.536 MHz, on", true, M_MOD_CLOCK_SET, M_PARAM_CLOCK_SET, &clk,
	    sizeof(clk)) &&
	    m_param("the port 0x1006: I2S, master, 48 kHz, 16 bits, stereo, data line 0", false, M_MOD_DEV_INTERFACE,
	    M_PARAM_I2S_CONFIG, &cfg, sizeof(cfg)) &&
	    m_cmd("the port 0x1006, started", 0, M_OP_START, port, sizeof(port));
	if (ok) {
		m_edges("port running", hold);
		PE_parse_boot_argn("tfa", &tfa, sizeof(tfa));
		if (tfa) {
			if (raphael_tfa_start(say_line)) {
				say("mi2s: the amplifier is started; held for %u s with the port running\n", hold);
				delay_for_interval(hold * 1000, NSEC_PER_MSEC);
				m_edges("port and amplifier running", hold);
			}
			raphael_tfadsp_report();
			raphael_tfa_stop(say_line);
		}
		m_cmd("the port 0x1006, stopped", 0, M_OP_STOP, port, sizeof(port));
	}
	clk.enable = 0;
	m_param("the bit clock, off", true, M_MOD_CLOCK_SET, M_PARAM_CLOCK_SET, &clk, sizeof(clk));
	/* back to what the device tree calls sleep: plain GPIO inputs, pulled down, 2 mA */
	m_pin(137, 0, 1, 0, false, false);
	m_pin(138, 0, 1, 0, false, false);
	m_pin(139, 0, 1, 0, false, false);
	m_pin(140, 0, 1, 0, false, false);
	say("mi2s: %s\n", ok ? "port opened, clock seen above; closed again" : "FAILED");
	return ok;
}
