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
#include <kern/locks.h>
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

static bool m_param_port(const char *what, uint16_t port, uint32_t module, uint32_t param, const void *data, uint16_t size);

#define M_PORT_TX               0x1007u         /* QUAT_MI2S_TX: the amplifier's current and voltage sense, data line 1 (GPIO 140) */
#define M_MOD_TFADSP_TX         0x1000b912u
#define M_PARAM_TFADSP_TX_ENABLE 0x1000b920u
#define M_PARAM_TFADSP_RX_BYPASS 0x1000b923u

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

/* A parameter of another port (in band, version 2 header). */
static bool
m_param_port(const char *what, uint16_t port, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	uint8_t packet[16 + sizeof(struct m_param) + 32];
	struct m_param *p = (struct m_param *)(packet + 16);

	bzero(packet, sizeof(packet));
	*(uint16_t *)packet = port;
	*(uint16_t *)(packet + 2) = (uint16_t)(sizeof(*p) + size);
	p->module = module;
	p->param = param;
	p->size = size;
	memcpy(packet + 16 + sizeof(*p), data, size);
	return m_cmd(what, port, M_OP_SET_PARAM_V2, packet, 16 + (uint32_t)sizeof(*p) + size);
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
#define M_INBAND_BUFFER         1024u
static uint32_t inband_max = 72;            /* boot-arg tfa_inband=N: the longest message sent in band (at most 1024) */

static int tfadsp_version = 3;
static uint32_t tfadsp_force_outband;      /* boot-arg tfa_oob=1: every message through memory (a test) */
static uintptr_t space_va;
static uint64_t space_dsp;
static uint32_t space_bytes, space_handle;
static bool space_held, space_mapped;

extern bool hakios_audio_dsp_memory(uintptr_t *memory, uint64_t *dsp_address, uint32_t *bytes);
extern void hakios_audio_dsp_memory_put(void);

/* the DSP reads and writes memory, not our caches: clean (and invalidate) the lines before it looks, and after it has written */
static void
tfadsp_flush(void)
{
	for (uintptr_t line = space_va; line < space_va + space_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

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
	space_bytes = 4096;                    /* one page, as Android maps for this */
	map.size = space_bytes;
	if (!hakios_adsp_apr(M_AFE, 0, 0, M_OP_MAP, &map, sizeof(map), M_WAIT_MS, &a) || a.opcode != M_RSP_MAP) {
		say("mi2s: the AFE did not map our memory: %s %#x\n", a.opcode == M_RESULT ? "status" : "opcode",
		    a.opcode == M_RESULT ? a.words[1] : a.opcode);
		return false;
	}
	space_handle = a.words[0];
	space_mapped = true;
	say("mi2s: the AFE has mapped it: handle %#x (the answer: %u bytes, %#x %#x %#x)\n", space_handle, a.length, a.words[0], a.words[1],
	    a.words[2]);
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
	uint8_t packet[24 + sizeof(struct m_param3) + M_INBAND_BUFFER];
	unsigned padded = (length + 3u) & ~3u;
	unsigned header = version == 3 ? (unsigned)sizeof(struct m_param3) : (unsigned)sizeof(struct m_param);
	bool inband = padded <= inband_max && !tfadsp_force_outband;
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
	if (!inband) {
		tfadsp_flush();
	}
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
		/* (the DSP takes the in-band message with the version 3 header; through memory the version 2 command works) */
		bool outband = ((length + 3u) & ~3u) > inband_max || tfadsp_force_outband;

		sent = tfadsp_set(outband ? 2 : tfadsp_version, message, length, &a);
		if (sent && a.opcode == M_RESULT && a.words[1] == 0) {
			if (tfadsp_sent == 1) {
				say("mi2s: the DSP takes TFADSP messages with parameter header version %d\n", tfadsp_version);
			}
			delay_for_interval(5, NSEC_PER_MSEC);      /* (Android waits 2 ms after each) */
			return 0;
		}
		if (tfadsp_version == 3 && tfadsp_sent == 1 && !outband && sent && a.opcode == M_RESULT && a.words[1] != 0) {
			say("mi2s: version 3 refused (status %u); trying version 2\n", a.words[1]);
			tfadsp_version = 2;
			continue;
		}
		tfadsp_refused++;
		if (sent) {
			say("mi2s: the answer: opcode %#x, %u bytes: %#x %#x %#x %#x\n", a.opcode, a.length, a.words[0], a.words[1], a.words[2],
			    a.words[3]);
		}
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
	tfadsp_flush();
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
	tfadsp_flush();
	memcpy(result, (const uint8_t *)space_va + header, length);
	return 0;
}

void
raphael_tfadsp_report(void)
{
	say("mi2s: TFADSP: %u messages sent, %u refused or unanswered, %u reads\n", tfadsp_sent, tfadsp_refused, tfadsp_read);
}

extern void raphael_tfadsp_close(void);
extern void raphael_tfa_dump(const char *when, void (*say)(const char *));
extern kern_return_t thread_terminate(thread_t thread);
static void say_line(const char *text);

/* during the tone: a look at the data pin and the amplifier, from a thread of its own */
static void
look_thread(__unused void *parameter, __unused wait_result_t result)
{
	delay_for_interval(4000, NSEC_PER_MSEC);
	m_edges("DURING THE TONE", 0);
	raphael_tfa_dump("during the tone", say_line);
	thread_terminate(current_thread());
}
extern bool hakios_raphael_tone(uint32_t ms, uint32_t gain, uint32_t ramp_ms);
extern bool raphael_tfa_start(void (*say)(const char *));
extern void raphael_tfa_stop(void (*say)(const char *));

static void
say_line(const char *text)
{
	say("%s", text);
}

/* What is up, for raphael_mi2s_close(): the pins, the clock, the ports, the feedback, the amplifier. */
static struct {
	bool pins, clock, rx, tx, tx_enabled, amplifier;
} up;
static LCK_GRP_DECLARE(amp_lock_group, "raphael-amp");
static LCK_MTX_DECLARE(amp_lock, &amp_lock_group);      /* the amplifier is started or stopped by one at a time */

/* The amplifier on (started from its container, the protection messages sent) or off; true if it is as asked. */
bool
raphael_amp_power(bool on)
{
	bool ok = true;

	lck_mtx_lock(&amp_lock);
	if (on && !up.amplifier) {
		ok = raphael_tfa_start(say_line);
		if (ok) {
			up.amplifier = true;
		} else {
			raphael_tfa_stop(say_line);
		}
	} else if (!on && up.amplifier) {
		raphael_tfa_stop(say_line);
		up.amplifier = false;
	}
	lck_mtx_unlock(&amp_lock);
	return ok;
}

/* Everything the step started, stopped in the opposite order; what is not up is left alone. */
void
raphael_mi2s_close(void)
{
	uint16_t port[2] = { M_PORT, 0 }, port_tx[2] = { M_PORT_TX, 0 };
	struct m_clock clk = { 1, M_CLK_QUAD_MI2S_IBIT, M_RATE * 32 * 2, M_CLK_COUPLE_NO, 0, 0 };
	uint32_t enable = 0;

	lck_mtx_lock(&amp_lock);
	if (up.amplifier) {
		raphael_tfa_stop(say_line);
		up.amplifier = false;
	}
	lck_mtx_unlock(&amp_lock);
	raphael_tfadsp_report();
	raphael_tfadsp_close();
	if (up.tx_enabled) {
		m_param_port("TFADSP_TX, disabled", M_PORT_TX, M_MOD_TFADSP_TX, M_PARAM_TFADSP_TX_ENABLE, &enable, sizeof(enable));
		up.tx_enabled = false;
	}
	if (up.tx) {
		m_cmd("the port 0x1007, stopped", 0, M_OP_STOP, port_tx, sizeof(port_tx));
		up.tx = false;
	}
	if (up.rx) {
		m_cmd("the port 0x1006, stopped", 0, M_OP_STOP, port, sizeof(port));
		up.rx = false;
	}
	if (up.clock) {
		m_param("the bit clock, off", true, M_MOD_CLOCK_SET, M_PARAM_CLOCK_SET, &clk, sizeof(clk));
		up.clock = false;
	}
	if (up.pins) {
		/* back to what the device tree calls sleep: plain GPIO inputs, pulled down, 2 mA */
		m_pin(137, 0, 1, 0, false, false);
		m_pin(138, 0, 1, 0, false, false);
		m_pin(139, 0, 1, 0, false, false);
		m_pin(140, 0, 1, 0, false, false);
		up.pins = false;
	}
}


/*
 * Around an amplifier power-cycle (the beeps test, hakios.sound.c): what is done to the DSP's side. Bits of `variant':
 * 1 the feedback module (TFADSP_TX) disabled, then enabled again;  2 the protection (TFADSP_RX) bypassed, then not;
 * 4 the feedback port (0x1007) stopped and started again;  8 the playback port (0x1006) stopped and started again;
 * 16 the stream restarted (done in hakios.sound.c).
 */
void
raphael_cycle_before(uint32_t variant)
{
	uint16_t port[2] = { M_PORT, 0 }, port_tx[2] = { M_PORT_TX, 0 };
	uint32_t zero = 0, one = 1;

	if (variant & 1) {
		m_param_port("variant: TFADSP_TX, disabled", M_PORT_TX, M_MOD_TFADSP_TX, M_PARAM_TFADSP_TX_ENABLE, &zero, sizeof(zero));
	}
	if (variant & 2) {
		m_param_port("variant: TFADSP_RX, bypassed", M_PORT, M_MOD_TFADSP_RX, M_PARAM_TFADSP_RX_BYPASS, &one, sizeof(one));
	}
	if (variant & 4) {
		m_cmd("variant: the port 0x1007, stopped", 0, M_OP_STOP, port_tx, sizeof(port_tx));
	}
	if (variant & 8) {
		m_cmd("variant: the port 0x1006, stopped", 0, M_OP_STOP, port, sizeof(port));
	}
}

void
raphael_cycle_after(uint32_t variant)
{
	uint16_t port[2] = { M_PORT, 0 }, port_tx[2] = { M_PORT_TX, 0 };
	uint32_t zero = 0, one = 1;

	if (variant & 8) {
		m_cmd("variant: the port 0x1006, started", 0, M_OP_START, port, sizeof(port));
	}
	if (variant & 4) {
		m_cmd("variant: the port 0x1007, started", 0, M_OP_START, port_tx, sizeof(port_tx));
	}
	if (variant & 1) {
		m_param_port("variant: TFADSP_TX, enabled", M_PORT_TX, M_MOD_TFADSP_TX, M_PARAM_TFADSP_TX_ENABLE, &one, sizeof(one));
	} else if (variant & 4) {
		m_param_port("variant: TFADSP_TX, enabled", M_PORT_TX, M_MOD_TFADSP_TX, M_PARAM_TFADSP_TX_ENABLE, &one, sizeof(one));
	}
	if (variant & 2) {
		m_param_port("variant: TFADSP_RX, not bypassed", M_PORT, M_MOD_TFADSP_RX, M_PARAM_TFADSP_RX_BYPASS, &zero, sizeof(zero));
	}
}

extern void hakios_sound_dsp_down(void);
#define raphael_ios_stop() hakios_sound_dsp_down()
extern bool hakios_raphael_beeps(uint32_t count, uint32_t on_ms, uint32_t off_ms, uint32_t gain, uint32_t variant);

extern bool hakios_raphael_ios_start(void);
extern void hakios_raphael_ios_selftest(uint32_t bursts, uint32_t burst_ms, uint32_t gap_ms);

bool
raphael_mi2s_step(void)
{
	uint32_t hold = 3, tfa = 0, ios = 0;
	struct m_clock clk = { 1, M_CLK_QUAD_MI2S_IBIT, M_RATE * 32 * 2, M_CLK_COUPLE_NO, 0, 1 };
	struct m_i2s cfg = { .version = 1, .bit_width = 24, .channel_mode = 1 /* SD0 */, .mono_stereo = 1 /* stereo */,
		             .ws_src = 1 /* internal: we are the master */, .sample_rate = M_RATE, .data_format = 0 };
	uint16_t port[2] = { M_PORT, 0 }, port_tx[2] = { M_PORT_TX, 0 };
	uint32_t tx = 0, enable = 1, bypass = 0, tone_ms = 3000, tone_gain_arg = 4, tone_ramp_ms = 1000;
	uint32_t tone_repeat = 1, beeps = 0, beeps_off_ms = 4000, beeps_variant = 0;
	bool ok, tone_gain_set, keep = false;

	if (!hakios_is_raphael()) {
		return true;
	}
	PE_parse_boot_argn("mi2s_hold", &hold, sizeof(hold));
	tone_gain_set = PE_parse_boot_argn("tone", &tone_ms, sizeof(tone_ms)) && tone_ms != 0;      /* boot-arg tone=MS plays a tone that long */
	PE_parse_boot_argn("tone_gain", &tone_gain_arg, sizeof(tone_gain_arg));
	PE_parse_boot_argn("tone_ramp_ms", &tone_ramp_ms, sizeof(tone_ramp_ms));
	PE_parse_boot_argn("tone_repeat", &tone_repeat, sizeof(tone_repeat));
	PE_parse_boot_argn("tone_beeps", &beeps, sizeof(beeps));
	PE_parse_boot_argn("tone_off_ms", &beeps_off_ms, sizeof(beeps_off_ms));
	PE_parse_boot_argn("beeps_variant", &beeps_variant, sizeof(beeps_variant));
	PE_parse_boot_argn("tfa_oob", &tfadsp_force_outband, sizeof(tfadsp_force_outband));
	PE_parse_boot_argn("tfa_inband", &inband_max, sizeof(inband_max));
	if (inband_max > M_INBAND_BUFFER) {
		inband_max = M_INBAND_BUFFER;
	}
	PE_parse_boot_argn("tfa", &tfa, sizeof(tfa));
	PE_parse_boot_argn("tfa_tx", &tx, sizeof(tx));
	PE_parse_boot_argn("ios_audio", &ios, sizeof(ios));     /* boot-arg ios_audio=1 (with tfa=1 tfa_tx=1 adsp_keep=1): what iOS plays goes to the speaker */
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
	up.pins = true;
	say("mi2s: pins on the interface: 137=%#x 138=%#x 139=%#x 140=%#x\n", m_rd(137 * 0x1000u), m_rd(138 * 0x1000u),
	    m_rd(139 * 0x1000u), m_rd(140 * 0x1000u));
	ok = m_param("the quaternary MI2S bit clock (0x106) at 3.072 MHz (24 bits in 32-bit slots), on", true, M_MOD_CLOCK_SET,
	    M_PARAM_CLOCK_SET, &clk, sizeof(clk));
	up.clock = ok;
	ok = ok && m_param("the port 0x1006: I2S, master, 48 kHz, 24 bits (32-bit slots), stereo, data line 0", false, M_MOD_DEV_INTERFACE,
	    M_PARAM_I2S_CONFIG, &cfg, sizeof(cfg)) &&
	    m_cmd("the port 0x1006, started", 0, M_OP_START, port, sizeof(port));
	up.rx = ok;
	if (ok) {
		m_edges("port running", hold);
		if (tfa && tx) {
			/* the feedback: the same interface, data line 1 (SD1), the DSP's TFADSP_TX module switched on, the protection not bypassed */
			struct m_i2s cfg_tx = cfg;

			cfg_tx.channel_mode = 2;
			up.tx = m_param_port("the port 0x1007 (feedback): I2S, 48 kHz, 24 bits (32-bit slots), stereo, data line 1", M_PORT_TX,
			    M_MOD_DEV_INTERFACE, M_PARAM_I2S_CONFIG, &cfg_tx, sizeof(cfg_tx)) &&
			    m_cmd("the port 0x1007, started", 0, M_OP_START, port_tx, sizeof(port_tx));
			up.tx_enabled = up.tx &&
			    m_param_port("TFADSP_TX, enabled", M_PORT_TX, M_MOD_TFADSP_TX, M_PARAM_TFADSP_TX_ENABLE, &enable, sizeof(enable)) &&
			    m_param_port("TFADSP_RX, not bypassed", M_PORT, M_MOD_TFADSP_RX, M_PARAM_TFADSP_RX_BYPASS, &bypass, sizeof(bypass));
			m_edges("feedback port running", hold);
		}
		if (tfa) {
			if (raphael_amp_power(true)) {
				say("mi2s: the amplifier is started\n");
				if (tone_gain_set) {
					thread_t look;

					/* quiet by default: 4/256 of -20 dBFS; boot-arg tone_gain=N (at most 256), tone_ms, tone_ramp_ms */
					say("mi2s: the tone follows: %u ms, gain %u/256, rising over %u ms\n", tone_ms, tone_gain_arg, tone_ramp_ms);
					if (kernel_thread_start(look_thread, NULL, &look) == KERN_SUCCESS) {
						thread_deallocate(look);
					}
					raphael_tfa_dump("before the tone", say_line);
					if (beeps) {
						hakios_raphael_beeps(beeps, tone_ms, beeps_off_ms, tone_gain_arg, beeps_variant);
					} else {
						hakios_raphael_tone(tone_ms, tone_gain_arg, tone_ramp_ms);
					}
					m_edges("after the tone", hold);
					/* boot-arg tone_repeat=N: the amplifier stopped and started again N-1 times, a tone each time (a test) */
					for (uint32_t again = 1; again < tone_repeat; again++) {
						say("mi2s: ---- restart %u: the amplifier is powered down, 2 s, and started again\n", again);
						raphael_amp_power(false);
						delay_for_interval(2000, NSEC_PER_MSEC);
						if (!raphael_amp_power(true)) {
							break;
						}
						raphael_tfa_dump("after the restart", say_line);
						hakios_raphael_tone(tone_ms, tone_gain_arg, tone_ramp_ms);
					}
				} else if (!ios) {
					delay_for_interval(hold * 1000, NSEC_PER_MSEC);
					m_edges("port and amplifier running", hold);
				}
				if (ios && up.tx_enabled) {
					uint32_t selftest = 0, burst_ms = 2000, gap_ms = 8000;

					keep = hakios_raphael_ios_start();
					if (PE_parse_boot_argn("ios_selftest", &selftest, sizeof(selftest)) && selftest != 0 && keep) {
						PE_parse_boot_argn("ios_selftest_burst_ms", &burst_ms, sizeof(burst_ms));
						PE_parse_boot_argn("ios_selftest_gap_ms", &gap_ms, sizeof(gap_ms));
						hakios_raphael_ios_selftest(selftest, burst_ms, gap_ms);
						keep = false;           /* (the test is over: everything is stopped below) */
						raphael_ios_stop();
					}
				}
			}
		}
	}
	if (!keep) {
		raphael_mi2s_close();
	}
	say("mi2s: %s\n", !ok ? "FAILED" : keep ? "the port, the feedback and the amplifier STAY UP for iOS's sound" :
	    "port opened, clock seen above; closed again");
	return ok;
}
