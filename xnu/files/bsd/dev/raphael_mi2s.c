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
#define M_OP_SET_PARAM_V2      0x000100efu
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

/*
 * The DSP takes a message in band (inside the APR packet) only if it fits one of the intents it has given
 * (about 450 bytes of message); the larger ones go through memory it is shown, as Android does for all of
 * them: 16 pages of ours behind the DSP's addresses (hakios.sound.c, the microphones' share), mapped for the
 * AFE service, with the parameter header and the message written into it.
 * The in-band header version 3 (module, instance, parameter, size) is tried first; a "bad parameter" for it
 * means version 2 (module, parameter, size).
 */
#define M_OP_GET_PARAM_V2       0x000100f0u
#define M_RSP_GET_PARAM_V2      0x00010106u
#define M_OP_MAP                0x000100eau
#define M_RSP_MAP               0x000100ebu
#define M_OP_UNMAP              0x000100ecu
#define M_POOL_SHMEM8_4K        3u
#define M_INBAND_MAX            384u

static int tfadsp_version = 3;
static uintptr_t space_va;
static uint64_t space_dsp;
static uint32_t space_bytes, space_handle;
static bool space_held, space_mapped;

extern bool hakios_audio_dsp_memory(uintptr_t *memory, uint64_t *dsp_address, uint32_t *bytes);
extern void hakios_audio_dsp_memory_put(void);

static bool
tfadsp_space(void)
{
	struct {
		uint16_t pool, regions;
		uint32_t property;
		uint32_t low, high, size;
	} __attribute__((packed)) map;
	struct hakios_apr_answer a;

	if (space_mapped) {
		return true;
	}
	if (!space_held) {
		if (!hakios_audio_dsp_memory(&space_va, &space_dsp, &space_bytes)) {
			say("mi2s: no memory the DSP can reach (the SMMU stream is not set up)\n");
			return false;
		}
		space_held = true;
		say("mi2s: %u bytes of ours at the DSP's address %#llx\n", space_bytes, space_dsp);
	}
	map.pool = M_POOL_SHMEM8_4K;
	map.regions = 1;
	map.property = 0;
	map.low = (uint32_t)space_dsp;
	map.high = (uint32_t)(space_dsp >> 32);
	map.size = space_bytes;
	if (!hakios_adsp_apr(M_AFE, 0, 0, M_OP_MAP, &map, sizeof(map), M_WAIT_MS, &a) || a.opcode != M_RSP_MAP) {
		say("mi2s: the AFE did not map our memory: %s %#x\n", a.opcode == M_RESULT ? "status" : "opcode",
		    a.opcode == M_RESULT ? a.words[1] : a.opcode);
		return false;
	}
	space_handle = a.words[0];
	space_mapped = true;
	say("mi2s: the AFE has mapped it: handle %#x\n", space_handle);
	return true;
}

void
raphael_tfadsp_close(void)
{
	if (space_mapped) {
		uint32_t handle = space_handle;

		hakios_adsp_apr(M_AFE, 0, 0, M_OP_UNMAP, &handle, sizeof(handle), M_WAIT_MS, NULL);
		space_mapped = false;
	}
	if (space_held) {
		hakios_audio_dsp_memory_put();
		space_held = false;
	}
}

/*
 * One SET_PARAM for the TFADSP_RX module, `version' 3 (command 0x100fa: port, memory header, payload size; the
 * parameter has a header with an instance id) or 2 (0x100ef: port, payload size, memory header; header without).
 * In band if it is short (inband), else in memory of ours.
 */
static bool
tfadsp_set(int version, const char *message, unsigned length, struct hakios_apr_answer *a)
{
	uint8_t packet[24 + sizeof(struct m_param3) + M_INBAND_MAX];
	unsigned padded = (length + 3u) & ~3u;
	unsigned header = version == 3 ? (unsigned)sizeof(struct m_param3) : (unsigned)sizeof(struct m_param);
	bool inband = padded <= M_INBAND_MAX;
	uint8_t *param;
	unsigned command_bytes = version == 3 ? 20 : 16;
	uint64_t address = 0;
	uint32_t handle = 0;

	if (inband) {
		bzero(packet, sizeof(packet));
		param = packet + command_bytes;
	} else {
		if (!tfadsp_space() || header + padded > space_bytes) {
			return false;
		}
		bzero((void *)space_va, header + padded);
		param = (uint8_t *)space_va;
		address = space_dsp;
		handle = space_handle;
		bzero(packet, command_bytes);
	}
	if (version == 3) {
		struct m_param3 *p = (struct m_param3 *)param;

		p->module = M_MOD_TFADSP_RX;
		p->param = M_PARAM_TFADSP_RX_CFG;
		p->size = padded;
	} else {
		struct m_param *p = (struct m_param *)param;

		p->module = M_MOD_TFADSP_RX;
		p->param = M_PARAM_TFADSP_RX_CFG;
		p->size = (uint16_t)padded;
	}
	memcpy(param + header, message, length);
	if (version == 3) {
		*(uint16_t *)packet = M_PORT;
		*(uint32_t *)(packet + 4) = (uint32_t)address;
		*(uint32_t *)(packet + 8) = (uint32_t)(address >> 32);
		*(uint32_t *)(packet + 12) = handle;
		*(uint32_t *)(packet + 16) = header + padded;
	} else {
		*(uint16_t *)packet = M_PORT;
		*(uint16_t *)(packet + 2) = (uint16_t)(header + padded);
		*(uint32_t *)(packet + 4) = (uint32_t)address;
		*(uint32_t *)(packet + 8) = (uint32_t)(address >> 32);
		*(uint32_t *)(packet + 12) = handle;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	return hakios_adsp_apr(M_AFE, M_PORT, 0, version == 3 ? M_OP_SET_PARAM_V3 : M_OP_SET_PARAM_V2, packet,
	           inband ? command_bytes + header + padded : command_bytes, M_WAIT_MS, a);
}

int
raphael_tfadsp_write(const char *message, unsigned length)
{
	struct hakios_apr_answer a;
	bool sent;

	tfadsp_sent++;
	say("mi2s: TFADSP message %u: %u bytes, %02x %02x %02x %02x %02x %02x\n", tfadsp_sent, length, (uint8_t)message[0],
	    (uint8_t)message[1], (uint8_t)message[2], (uint8_t)message[3], (uint8_t)message[4], (uint8_t)message[5]);
	for (;;) {
		sent = tfadsp_set(tfadsp_version, message, length, &a);
		if (sent && a.opcode == M_RESULT && a.words[1] == 0) {
			if (tfadsp_sent == 1) {
				say("mi2s: the DSP takes TFADSP messages with parameter header version %d\n", tfadsp_version);
			}
			delay_for_interval(5, NSEC_PER_MSEC);      /* (Android waits 2 ms after each) */
			return 0;
		}
		if (tfadsp_version == 3 && tfadsp_sent == 1 && sent && a.opcode == M_RESULT && a.words[1] != 0) {
			say("mi2s: version 3 refused (status %u); trying version 2\n", a.words[1]);
			tfadsp_version = 2;
			continue;
		}
		tfadsp_refused++;
		say("mi2s: TFADSP message %u (%u bytes, %02x %02x %02x): %s %#x\n", tfadsp_sent, length, (uint8_t)message[0],
		    (uint8_t)message[1], (uint8_t)message[2], !sent ? "not sent / no answer, opcode" :
		    a.opcode == M_RESULT ? "REFUSED, status" : "unexpected opcode", !sent ? a.opcode : a.opcode == M_RESULT ? a.words[1] : a.opcode);
		return 1;
	}
}

/* the result of the last message: asked for through memory of ours, answered into it (header, then the data) */
int
raphael_tfadsp_read(uint8_t *result, unsigned length)
{
	uint8_t packet[40];
	unsigned padded = (length + 3u) & ~3u;
	unsigned header = tfadsp_version == 3 ? (unsigned)sizeof(struct m_param3) : (unsigned)sizeof(struct m_param);
	unsigned bytes;
	struct hakios_apr_answer a;

	tfadsp_read++;
	if (!tfadsp_space() || header + padded > space_bytes) {
		return 1;
	}
	bzero((void *)space_va, header + padded);
	bzero(packet, sizeof(packet));
	__asm__ volatile ("dsb sy" ::: "memory");
	*(uint16_t *)packet = M_PORT;
	if (tfadsp_version == 3) {
		struct m_param3 *p = (struct m_param3 *)(packet + 16);

		*(uint32_t *)(packet + 4) = (uint32_t)space_dsp;
		*(uint32_t *)(packet + 8) = (uint32_t)(space_dsp >> 32);
		*(uint32_t *)(packet + 12) = space_handle;
		p->module = M_MOD_TFADSP_RX;
		p->param = M_PARAM_TFADSP_RX_GET;
		p->size = padded;
		bytes = 16 + (unsigned)sizeof(*p);
	} else {
		struct m_param *p = (struct m_param *)(packet + 28);

		*(uint16_t *)(packet + 2) = (uint16_t)padded;
		*(uint32_t *)(packet + 4) = (uint32_t)space_dsp;
		*(uint32_t *)(packet + 8) = (uint32_t)(space_dsp >> 32);
		*(uint32_t *)(packet + 12) = space_handle;
		*(uint32_t *)(packet + 16) = M_MOD_TFADSP_RX;
		*(uint32_t *)(packet + 20) = M_PARAM_TFADSP_RX_GET;
		p->module = M_MOD_TFADSP_RX;
		p->param = M_PARAM_TFADSP_RX_GET;
		p->size = (uint16_t)padded;
		bytes = 28 + (unsigned)sizeof(*p);
	}
	if (!hakios_adsp_apr(M_AFE, M_PORT, 0, tfadsp_version == 3 ? M_OP_GET_PARAM_V3 : M_OP_GET_PARAM_V2, packet, bytes,
	    M_WAIT_MS, &a) || (a.opcode == M_RESULT && a.words[1] != 0) ||
	    (a.opcode != M_RESULT && a.opcode != M_RSP_GET_PARAM_V2 && a.opcode != M_RSP_GET_PARAM_V3)) {
		say("mi2s: TFADSP read %u (%u bytes): %s %#x\n", tfadsp_read, length, a.opcode == M_RESULT ? "refused, status" :
		    "no answer / opcode", a.opcode == M_RESULT ? a.words[1] : a.opcode);
		return 1;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	memcpy(result, (const uint8_t *)space_va + header, length);
	return 0;
}

void
raphael_tfadsp_report(void)
{
	say("mi2s: TFADSP: %u messages sent, %u refused or unanswered, %u reads\n", tfadsp_sent, tfadsp_refused, tfadsp_read);
}

extern void raphael_tfadsp_close(void);
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
			raphael_tfadsp_close();
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
