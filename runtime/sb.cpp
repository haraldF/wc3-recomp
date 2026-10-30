/* Sound Blaster 16 digital sound: the DSP at ports 220h-22Fh, IRQ 5, 8-bit DMA channel 1 and 16-bit
 * channel 5, as GOG's WC3.CFG has it, through the two 8237 DMA controllers (channels 0-3 at ports
 * 00h-0Fh, 4-7 at C0h-DEh, and the page registers). DSP 4.05: reset (2x6h, then AAh at 2xAh), the
 * original 8-bit mono output in single-cycle (14h), auto-initialize (1Ch) and direct (10h) mode at
 * the rate of a time constant (40h); the SB16's 8- and 16-bit transfers (Cxh, Bxh: mono or stereo,
 * signed or not, single-cycle or auto-initialize) at the rate of 41h; speaker (on an SB16 only its
 * status: the output is always on, and Wing Commander III never turns it on), pause, version; the
 * mixer's IRQ and DMA settings. A block plays in emulated time, its end raises the IRQ
 * (acknowledged at 2xEh for 8-bit, 2xFh for 16-bit), and sound.cpp mixes the samples into whatever
 * has the output (AdLib or MT-32), by time: sb_mix. WC_SB=0 removes the card. The FM chip of the
 * card is the AdLib's (opl.cpp). */
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "wc_rt.h"

uint64_t pc_now_ns(void);
void opl_poll(uint64_t now);
void mt32_poll(uint64_t now);
extern int wc_irq_pending;

#define SB_BASE 0x220
#define SB_IRQ 5
#define SB_DMA 1
#define SB_HDMA 5

static int present = 1;

/* ---- DMA controllers: channels 0-3 move bytes, 4-7 words ---- */

static struct { uint16_t base_addr, base_count, addr, count; uint8_t page, mode; } dma[8];
static struct { uint8_t mask = 0x0f, status, flipflop; } ctrl[2];

/* The channel whose page register is at port, else -1 */
static int dma_page_channel(uint64_t port)
{
	switch (port) {
	case 0x87: return 0;
	case 0x83: return 1;
	case 0x81: return 2;
	case 0x82: return 3;
	case 0x8b: return 5;
	case 0x89: return 6;
	case 0x8a: return 7;
	}
	return -1;
}

/* The controller register (0-15) at port, and its controller, else -1 */
static int dma_register(uint64_t port, int *k)
{
	if (port <= 0x0f) {
		*k = 0;
		return (int)port;
	}
	if (port >= 0xc0 && port <= 0xde && !(port & 1)) {
		*k = 1;
		return (int)(port - 0xc0) / 2;
	}
	return -1;
}

/* Linear address of a channel's unit i (bytes on 0-3, words on 4-7) */
static uint32_t dma_linear(int c, uint32_t i)
{
	if (c < 4) return (uint32_t)dma[c].page << 16 | (uint16_t)(dma[c].addr + i);
	return ((uint32_t)(dma[c].page & 0xfe) << 16) | (uint32_t)(uint16_t)(dma[c].addr + i) << 1;
}

/* ---- DSP ---- */

static uint8_t replies[16];
static unsigned replies_len;
static uint8_t cmd, args[3];
static unsigned args_have, args_need;
static uint8_t time_constant = 0xa5;   /* about 11 kHz */
static uint32_t out_rate = 22050;      /* 41h */
static int speaker;
static uint8_t mixer_index, mixer[256];
static uint8_t irq_status;             /* bit 0: 8-bit transfer, bit 1: 16-bit transfer */

/* the block being played */
static int playing, paused, auto_init;
static int bits16, stereo, is_signed, channel = SB_DMA;
static uint32_t rate, units, block_len = 0x800; /* units: bytes or words; block_len: for 1Ch */
static uint64_t start_ns, paused_at_ns;
static int16_t direct;                   /* the last sample of 10h */

static void reply(uint8_t v)
{
	if (replies_len < sizeof replies) replies[replies_len++] = v;
}

/* The samples up to now are mixed with the state they were played with. */
static void flush(uint64_t now)
{
	opl_poll(now);
	mt32_poll(now);
}

/* units per second */
static uint32_t unit_rate(void)
{
	return rate * (stereo ? 2 : 1);
}

static uint64_t block_end_ns(void)
{
	return start_ns + (uint64_t)units * 1000000000u / unit_rate();
}

static void start_block(uint64_t now, uint32_t len)
{
	units = len;
	start_ns = now;
	playing = 1;
	paused = 0;
	if (wc_trace) {
		fprintf(stderr, "wc: sb: %u %s at %u Hz%s from %05x\n", len, bits16 ? "words" : "bytes", rate,
			stereo ? " stereo" : "", dma_linear(channel, 0));
	}
}

/* The original 8-bit mono transfers, at the time constant's rate */
static void start_classic(uint64_t now, uint32_t len)
{
	bits16 = stereo = is_signed = 0;
	channel = SB_DMA;
	rate = 1000000u / (256u - time_constant);
	start_block(now, len);
}

/* The DMA channel's address and count after a block (the program may read them back). */
static void dma_advance(uint32_t n)
{
	int c = channel, k = c / 4;
	dma[c].addr += (uint16_t)n;
	dma[c].count -= (uint16_t)n;
	ctrl[k].status |= (uint8_t)(1 << (c & 3)); /* terminal count */
	if (dma[c].mode & 0x10) { /* auto-initialize */
		dma[c].addr = dma[c].base_addr;
		dma[c].count = dma[c].base_count;
	}
}

static void pause(uint64_t now)
{
	if (playing && !paused) {
		paused = 1;
		paused_at_ns = now;
	}
}

static void resume(uint64_t now)
{
	if (playing && paused) {
		start_ns += now - paused_at_ns;
		paused = 0;
	}
}

static void command(uint64_t now)
{
	if (cmd >= 0xb0 && cmd <= 0xcf) { /* SB16 transfer: Bxh 16-bit, Cxh 8-bit; mode, length - 1 */
		if (cmd & 0x08) return; /* input */
		bits16 = cmd < 0xc0;
		auto_init = (cmd & 0x04) != 0;
		is_signed = (args[0] & 0x10) != 0;
		stereo = (args[0] & 0x20) != 0;
		channel = bits16 ? SB_HDMA : SB_DMA;
		rate = out_rate;
		block_len = (uint32_t)(args[1] | args[2] << 8) + 1;
		start_block(now, block_len);
		return;
	}
	switch (cmd) {
	case 0x10: /* direct output: one sample */
		direct = (int16_t)((args[0] - 128) << 8);
		break;
	case 0x14: /* 8-bit single-cycle DMA output, length - 1 */
		start_classic(now, (uint32_t)(args[0] | args[1] << 8) + 1);
		auto_init = 0;
		break;
	case 0x1c: /* 8-bit auto-initialize DMA output, the block size of 48h */
		start_classic(now, block_len);
		auto_init = 1;
		break;
	case 0x48: /* block size for auto-initialize */
		block_len = (uint32_t)(args[0] | args[1] << 8) + 1;
		break;
	case 0x20: /* direct input: silence */
		reply(0x80);
		break;
	case 0x40: time_constant = args[0]; break;
	case 0x41: case 0x42: /* output (input) rate in Hz, high byte first */
		out_rate = (uint32_t)(args[0] << 8 | args[1]);
		if (!out_rate) out_rate = 5000;
		break;
	case 0xd0: case 0xd5: pause(now); break;     /* 8-bit, 16-bit */
	case 0xd4: case 0xd6: resume(now); break;
	case 0xd9: case 0xda: auto_init = 0; break;  /* exit auto-initialize after this block */
	case 0xd1: speaker = 1; break;
	case 0xd3: speaker = 0; break;
	case 0xd8: reply(speaker ? 0xff : 0x00); break;
	case 0xe1: reply(4); reply(5); break; /* version 4.05: a Sound Blaster 16 */
	case 0xf2: /* raise the 8-bit interrupt */
		irq_status |= 1;
		wc_irq_pending |= 1 << SB_IRQ;
		break;
	case 0xf3: /* raise the 16-bit interrupt */
		irq_status |= 2;
		wc_irq_pending |= 1 << SB_IRQ;
		break;
	default:
		if (wc_trace) fprintf(stderr, "wc: sb: unimplemented DSP command %02x\n", cmd);
		break;
	}
}

static unsigned arg_count(uint8_t c)
{
	if (c >= 0xb0 && c <= 0xcf) return 3;
	switch (c) {
	case 0x10: case 0x40: case 0x38: case 0xe0: case 0xe4: return 1;
	case 0x14: case 0x16: case 0x17: case 0x24: case 0x41: case 0x42: case 0x48:
	case 0x74: case 0x75: case 0x76: case 0x77: return 2;
	}
	return 0;
}

static void dsp_write(uint8_t v)
{
	uint64_t now = pc_now_ns();
	flush(now);
	if (args_need) {
		args[args_have++] = v;
		if (args_have == args_need) {
			args_need = 0;
			command(now);
		}
		return;
	}
	cmd = v;
	args_have = 0;
	args_need = arg_count(v);
	if (!args_need) command(now);
}

static void dsp_reset(void)
{
	flush(pc_now_ns());
	playing = paused = auto_init = 0;
	speaker = 0;
	args_need = 0;
	direct = 0;
	replies_len = 0;
	reply(0xaa);
}

/* ---- the program's side ---- */

int sb_port(uint64_t port)
{
	int k;
	return present && ((port >= SB_BASE + 4 && port <= SB_BASE + 0xf) || dma_register(port, &k) >= 0 ||
		dma_page_channel(port) >= 0);
}

static uint8_t mixer_read(uint8_t r)
{
	switch (r) {
	case 0x80: return 0x02; /* IRQ 5 */
	case 0x81: return (uint8_t)(1 << SB_DMA | 1 << SB_HDMA);
	case 0x82: return irq_status;
	}
	return mixer[r];
}

uint64_t sb_in(uint64_t port)
{
	int k, r = dma_register(port, &k);
	if (r >= 0 && r < 8) { /* address and count of a channel, low byte then high byte */
		int c = k * 4 + r / 2;
		uint64_t now = pc_now_ns();
		uint16_t v = r & 1 ? dma[c].count : dma[c].addr;
		if (c == channel && playing && !paused && now < block_end_ns()) { /* where the block is now */
			uint16_t done = (uint16_t)((now - start_ns) * unit_rate() / 1000000000u);
			v = r & 1 ? (uint16_t)(v - done) : (uint16_t)(v + done);
		}
		uint8_t b = ctrl[k].flipflop ? v >> 8 : v & 0xff;
		ctrl[k].flipflop ^= 1;
		return b;
	}
	if (r == 8) {
		uint8_t s = ctrl[k].status;
		ctrl[k].status &= 0xf0; /* reading clears the terminal counts */
		return s;
	}
	if (r >= 0 || dma_page_channel(port) >= 0) return 0xff;
	switch (port - SB_BASE) {
	case 0x5: return mixer_read(mixer_index);
	case 0xa: /* read data */
		if (!replies_len) return 0xff;
		{
			uint8_t v = replies[0];
			memmove(replies, replies + 1, --replies_len);
			return v;
		}
	case 0xc: return 0x7f; /* write status: never busy */
	case 0xe: /* read status (data waiting); acknowledges the 8-bit interrupt */
		irq_status &= (uint8_t)~1;
		return replies_len ? 0xff : 0x7f;
	case 0xf: /* acknowledges the 16-bit interrupt */
		irq_status &= (uint8_t)~2;
		return 0xff;
	}
	return 0xff;
}

void sb_out(uint64_t port, uint64_t value)
{
	uint8_t v = (uint8_t)value;
	int k, r = dma_register(port, &k);
	if (r >= 0 && r < 8) {
		int c = k * 4 + r / 2;
		uint16_t *base = r & 1 ? &dma[c].base_count : &dma[c].base_addr;
		*base = ctrl[k].flipflop ? (uint16_t)((*base & 0x00ff) | v << 8) : (uint16_t)((*base & 0xff00) | v);
		if (r & 1) dma[c].count = *base;
		else dma[c].addr = *base;
		ctrl[k].flipflop ^= 1;
		return;
	}
	if (int c = dma_page_channel(port); c >= 0) {
		dma[c].page = v;
		return;
	}
	switch (r) {
	case 10: /* single mask bit */
		if (v & 4) ctrl[k].mask |= (uint8_t)(1 << (v & 3));
		else ctrl[k].mask &= (uint8_t)~(1 << (v & 3));
		return;
	case 11: dma[k * 4 + (v & 3)].mode = v; return;
	case 12: ctrl[k].flipflop = 0; return;
	case 13: ctrl[k].mask = 0x0f; ctrl[k].flipflop = 0; ctrl[k].status = 0; return; /* master clear */
	case 14: ctrl[k].mask = 0; return;
	case 15: ctrl[k].mask = v & 0x0f; return;
	}
	if (r >= 0) return;
	switch (port - SB_BASE) {
	case 0x4: mixer_index = v; break;
	case 0x5: mixer[mixer_index] = v; break;
	case 0x6:
		if (v & 1) dsp_reset();
		break;
	case 0xc: dsp_write(v); break;
	}
}

/* At the end of a block: advance the DMA channel, raise the interrupt, go on if auto-initialize. */
void sb_poll(uint64_t now)
{
	if (!playing || paused || now < block_end_ns()) return;
	uint64_t end = block_end_ns();
	dma_advance(units);
	irq_status |= bits16 ? 2 : 1;
	wc_irq_pending |= 1 << SB_IRQ;
	if (auto_init) start_block(end, block_len);
	else playing = 0;
}

/* Unit i of the block as a 16-bit sample at full scale +-16384 (the AdLib is far quieter) */
static int unit_sample(uint32_t i)
{
	uint32_t a = dma_linear(channel, i);
	if (bits16) {
		int v = M[a] | M[a + 1] << 8;
		return (is_signed ? (int16_t)v : v - 32768) >> 1;
	}
	return (is_signed ? (int8_t)M[a] : M[a] - 128) << 7;
}

/* The sample of channel ch (0 left, 1 right) at time t, linearly between the DMA's frames. */
static int sample_at(uint64_t t, int ch)
{
	if (!playing || paused || t < start_ns) return direct;
	if (ctrl[channel / 4].mask & (1 << (channel & 3))) return 0;
	int n = stereo ? 2 : 1;
	uint64_t pos = (t - start_ns) * rate * 256u / 1000000000u; /* in 1/256 frames */
	uint32_t f = (uint32_t)(pos >> 8), c = (uint32_t)(stereo ? ch : 0);
	if ((f + 1) * n > units) return 0;
	int s0 = unit_sample(f * n + c);
	int s1 = (f + 2) * n <= units ? unit_sample((f + 1) * n + c) : s0;
	int w = (int)(pos & 0xff);
	return (s0 * (256 - w) + s1 * w) >> 8;
}

/* Adds the card's output to n stereo frames at rate, the first one at time t0. */
void sb_mix(int16_t *frames, unsigned n, unsigned rate_out, uint64_t t0)
{
	if (!present || (!playing && !direct)) return;
	for (unsigned i = 0; i < n; i++) {
		uint64_t t = t0 + (uint64_t)i * 1000000000u / rate_out;
		for (int c = 0; c < 2; c++) {
			int v = frames[2 * i + c] + sample_at(t, c);
			frames[2 * i + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
	}
}

void sb_init(void)
{
	const char *on = getenv("WC_SB");
	present = !on || strcmp(on, "0");
}
