/* VGA: the DAC (palette), the status register's blanking and retrace bits in real time, and the
 * frames the host shows, copied from the frame buffer at each vertical retrace: mode 13h, or the
 * VESA (VBE 1.2) mode 101h, 640x480 with 256 colours, banked through the 64 KB window at A0000h
 * (Wing Commander III's VESA480.DLL; without it, MCGA.DLL at 320x200). WC_VESA=0: no VESA BIOS. */
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "host.h"
#include "wc_rt.h"

uint64_t pc_now_ns(void);
int pc_warping(void);
int pc_script_warp(void);
void pc_warp_end(void);

/* ---- DAC: index at 3C8h (write) / 3C7h (read), then R, G, B (6 bits) at 3C9h ---- */

uint8_t wc_palette[256][3];
static uint8_t dac_write, dac_read, dac_part;

/* ---- timing: mode 13h is 400 lines (200 doubled) of 449, at 31.78 us a line: 70.09 Hz ---- */

#define LINE_NS 31778u
#define LINES 449u
#define FRAME_NS ((uint64_t)LINE_NS * LINES)
#define RETRACE_LINE 412u /* vertical retrace: 2 lines, inside the blanking from line 400 */

static uint64_t start_ns;
static uint64_t retraces_seen; /* retraces the program has seen in the status register */

/* The program runs in bursts (the CPU speed limit sleeps in between), so it can miss the 64 us
 * retrace while polling: a retrace that began since the last read is reported once, so that a
 * "wait for retrace" loop still sees every frame. */
static uint8_t status(void)
{
	uint64_t since = pc_now_ns() - start_ns, r = (uint64_t)RETRACE_LINE * LINE_NS;
	uint64_t t = since % FRAME_NS;
	uint64_t retraces = since >= r ? (since - r) / FRAME_NS + 1 : 0; /* retraces begun so far */
	uint32_t line = (uint32_t)(t / LINE_NS), dot = (uint32_t)(t % LINE_NS);
	uint8_t v = 0;
	if (line >= 400 || dot >= LINE_NS * 4 / 5) v |= 0x01; /* blanking (display disabled) */
	if (line >= RETRACE_LINE && line < RETRACE_LINE + 2) {
		v |= 0x08;
		retraces_seen = retraces;
	}
	else if (retraces > retraces_seen) {
		v |= 0x09;
		retraces_seen = retraces;
	}
	return v;
}

/* ---- VESA: video memory beyond the window ---- */

enum { VRAM = 512 * 1024, BANK = 64 * 1024 };
static uint8_t vram[VRAM];
static int svga;        /* in mode 101h */
static unsigned bank;   /* the bank in the window at A0000h */
static uint32_t display_start; /* where the picture starts in video memory (4F07h) */
/* the CRT controller's and the sequencer's registers (3D4h/3D5h, 3C4h/3C5h): only the maximum scan
 * line (CRTC 9: bit 7 doubles every line, bits 0-4 repeat it) and screen off (sequencer 1, bit 5)
 * change the picture here; the game's movies at 640x480 are 240 lines, doubled */
static uint8_t crtc[32], crtc_index, seq[8], seq_index;

/* The window's contents go back to video memory (the program wrote them there). */
static void bank_flush(void)
{
	memcpy(vram + bank * BANK, M + 0xa0000, BANK);
}

static void bank_set(unsigned b)
{
	if (b >= VRAM / BANK || b == bank) return;
	bank_flush();
	bank = b;
	memcpy(M + 0xa0000, vram + bank * BANK, BANK);
}

/* The VESA window function (the mode's WinFuncPtr, F000:0100): BH 0 set the bank to DX, 1 get it. */
void vga_window_function(void)
{
	if ((rd(R_BX, 2) >> 8) == 0) bank_set((unsigned)rd(R_DX, 2));
	else wr(R_DX, 2, bank);
}

/* ---- frames for the host ---- */

/* The picture as it is now: its width and height, the pixels into buf (up to 640x480 bytes). */
int vga_picture(uint8_t *buf, int *w, int *h)
{
	if (svga) {
		bank_flush();
		*w = 640;
		*h = 480;
		int repeat = ((crtc[9] & 0x1f) + 1) * (crtc[9] & 0x80 ? 2 : 1);
		for (int y = 0; y < 480; y++) {
			uint32_t from = display_start + (uint32_t)(y / repeat) * 640;
			memcpy(buf + y * 640, vram + (from + 640 <= VRAM ? from : 0), 640);
		}
	}
	else {
		*w = 320;
		*h = 200;
		memcpy(buf, M + 0xa0000, 320 * 200);
	}
	if (seq[1] & 0x20) memset(buf, 0, (size_t)*w * *h); /* screen off */
	return 1;
}

static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t frame_pixels[640 * 480], frame_palette[256 * 3];
static int frame_w = 320, frame_h = 200;
static uint64_t frame_count, frame_taken;

/* Called regularly on the emulation's thread: at each retrace, copy the picture for the host. */
void vga_poll(uint64_t now)
{
	uint64_t frame = (now - start_ns + FRAME_NS - (uint64_t)RETRACE_LINE * LINE_NS) / FRAME_NS;
	if (frame == frame_taken) return;
	frame_taken = frame;
	pthread_mutex_lock(&frame_lock);
	vga_picture(frame_pixels, &frame_w, &frame_h);
	memcpy(frame_palette, wc_palette, sizeof frame_palette);
	frame_count++;
	pthread_mutex_unlock(&frame_lock);
	if (pc_warping() && !pc_script_warp()) { /* the quick start ends with the first visible picture */
		for (int i = 0; i < frame_w * frame_h; i++) {
			const uint8_t *c = frame_palette + 3 * frame_pixels[i];
			if (c[0] | c[1] | c[2]) {
				pc_warp_end();
				break;
			}
		}
	}
}

uint64_t vga_frame(uint8_t pixels[640 * 480], uint8_t palette[256 * 3], int *w, int *h)
{
	pthread_mutex_lock(&frame_lock);
	*w = frame_w;
	*h = frame_h;
	memcpy(pixels, frame_pixels, (size_t)frame_w * frame_h);
	memcpy(palette, frame_palette, sizeof frame_palette);
	uint64_t n = frame_count;
	pthread_mutex_unlock(&frame_lock);
	return n;
}

/* ---- ports ---- */

/* the Miscellaneous Output register: bit 0 set puts the CRT controller at 3Dxh (colour), where
 * drivers look for the retrace (MCGA.DLL reads it to choose 3BAh or 3DAh) */
static uint8_t misc_output = 0x63;

uint64_t vga_in(uint64_t port)
{
	uint64_t v = 0;
	switch (port) {
	case 0x3da: v = status(); break;
	case 0x3cc: v = misc_output; break;
	case 0x3c4: v = seq_index; break;
	case 0x3c5: v = seq[seq_index & 7]; break;
	case 0x3d4: v = crtc_index; break;
	case 0x3d5: v = crtc[crtc_index & 31]; break;
	case 0x3c9:
		v = wc_palette[dac_read][dac_part];
		if (++dac_part == 3) { dac_part = 0; dac_read++; }
		break;
	}
	return v;
}

void vga_out(uint64_t port, uint64_t value)
{
	switch (port) {
	case 0x3c2: misc_output = (uint8_t)value; break;
	case 0x3c4: /* a word: index and data */
		seq_index = (uint8_t)value;
		if (value > 0xff) seq[seq_index & 7] = (uint8_t)(value >> 8);
		break;
	case 0x3c5: seq[seq_index & 7] = (uint8_t)value; break;
	case 0x3d4:
		crtc_index = (uint8_t)value;
		if (value > 0xff) crtc[crtc_index & 31] = (uint8_t)(value >> 8);
		break;
	case 0x3d5: crtc[crtc_index & 31] = (uint8_t)value; break;
	case 0x3c7: dac_read = (uint8_t)value; dac_part = 0; break;
	case 0x3c8: dac_write = (uint8_t)value; dac_part = 0; break;
	case 0x3c9:
		wc_palette[dac_write][dac_part] = (uint8_t)(value & 0x3f);
		if (++dac_part == 3) { dac_part = 0; dac_write++; }
		break;
	}
}

void vga_init(void)
{
	start_ns = pc_now_ns();
}

/* ---- video BIOS (INT 10h): modes and the DAC; text output is not shown ---- */


void vga_bios(uint32_t at);
extern int dos_real; /* called through Phar Lap's real-mode interrupt (2511h): ES is a segment */

/* VESA BIOS (VBE 1.2): mode 101h only. Its strings and mode list live in the BIOS segment. */
enum { VESA_OEM = 0xf000, VESA_MODES = 0xf020, VESA_WINFUNC = 0x0100 };

static void vesa(uint32_t at)
{
	uint8_t al = (uint8_t)rd(R_AL, 1);
	uint32_t buf = dos_real ? (uint32_t)rd(R_ES, 2) * 16 + (uint32_t)rd(R_DI, 2) : wc_sb[0] + (uint32_t)rd(R_DI, 4);
	const char *off = getenv("WC_VESA");
	if (off && !strcmp(off, "0")) {
		wr(R_AX, 2, 0x014f); /* failed */
		return;
	}
	wr(R_AX, 2, 0x004f);
	switch (al) {
	case 0x00: { /* controller information into ES:DI */
		static const char oem[] = "wc3-recomp VBE";
		memcpy(M + 0xf0000 + VESA_OEM, oem, sizeof oem);
		mwr(0xf0000 + VESA_MODES, 2, 0x101);
		mwr(0xf0000 + VESA_MODES + 2, 2, 0xffff);
		memset(M + buf, 0, 256);
		memcpy(M + buf, "VESA", 4);
		mwr(buf + 4, 2, 0x0102);
		mwr(buf + 6, 4, 0xf0000000u | VESA_OEM);
		mwr(buf + 10, 4, 0);                        /* capabilities: a 6-bit DAC */
		mwr(buf + 14, 4, 0xf0000000u | VESA_MODES);
		mwr(buf + 18, 2, VRAM / BANK);              /* memory in 64 KB blocks */
		break;
	}
	case 0x01: { /* mode information into ES:DI */
		if ((rd(R_CX, 2) & 0x1ff) != 0x101) {
			wr(R_AX, 2, 0x014f);
			break;
		}
		memset(M + buf, 0, 256);
		mwr(buf + 0, 2, 0x001b);  /* supported, extended information, colour, graphics */
		M[buf + 2] = 7;           /* window A: there, readable, writable */
		M[buf + 3] = 0;           /* no window B */
		mwr(buf + 4, 2, 64);      /* granularity and size in KB */
		mwr(buf + 6, 2, 64);
		mwr(buf + 8, 2, 0xa000);  /* window A's segment */
		mwr(buf + 12, 4, 0xf0000000u | VESA_WINFUNC);
		mwr(buf + 16, 2, 640);    /* bytes per line */
		mwr(buf + 18, 2, 640);
		mwr(buf + 20, 2, 480);
		M[buf + 22] = 8;          /* character cell */
		M[buf + 23] = 16;
		M[buf + 24] = 1;          /* planes */
		M[buf + 25] = 8;          /* bits per pixel */
		M[buf + 26] = 1;          /* banks */
		M[buf + 27] = 4;          /* packed pixel */
		M[buf + 29] = 0;          /* image pages - 1 */
		M[buf + 30] = 1;
		break;
	}
	case 0x02: { /* set mode BX (bit 15: keep the memory) */
		uint16_t mode = (uint16_t)rd(R_BX, 2);
		if ((mode & 0x1ff) != 0x101) {
			if (mode < 0x100) {
				wr(R_AX, 2, mode & 0xff);
				svga = 0;
				vga_bios(at);
				wr(R_AX, 2, 0x004f);
			}
			else wr(R_AX, 2, 0x014f);
			break;
		}
		svga = 1;
		bank = 0;
		display_start = 0;
		crtc[9] = 0x40; /* one scan line per line */
		if (!(mode & 0x8000)) {
			memset(vram, 0, sizeof vram);
			memset(M + 0xa0000, 0, BANK);
		}
		M[0x449] = 0x13; /* what get mode reports: a graphics mode */
		break;
	}
	case 0x03: /* current mode */
		wr(R_BX, 2, svga ? 0x101 : M[0x449]);
		break;
	case 0x05: /* the window's bank */
		vga_window_function();
		break;
	case 0x07: /* display start: BL 0 set to pixel CX of line DX, 1 get */
		if ((rd(R_BX, 1)) == 0 || rd(R_BX, 1) == 0x80) {
			uint32_t start = (uint32_t)rd(R_DX, 2) * 640 + (uint32_t)rd(R_CX, 2);
			display_start = start + 640 * 480 <= VRAM ? start : 0;
		}
		else if (rd(R_BX, 1) == 1) {
			wr(R_BX, 2, 0);
			wr(R_CX, 2, display_start % 640);
			wr(R_DX, 2, display_start / 640);
		}
		break;
	default:
		wr(R_AX, 2, 0x014f);
		break;
	}
	if (wc_trace) fprintf(stderr, "wc: VESA AX=4f%02x BX=%04llx CX=%04llx DX=%04llx -> AX=%04llx at %05x\n", al,
		rd(R_BX, 2), rd(R_CX, 2), rd(R_DX, 2), rd(R_AX, 2), at);
}

void vga_bios(uint32_t at)
{
	uint8_t ah = (uint8_t)rd(R_AH, 1), al = (uint8_t)rd(R_AL, 1);
	uint16_t bx = (uint16_t)rd(R_BX, 2), cx = (uint16_t)rd(R_CX, 2), dx = (uint16_t)rd(R_DX, 2);
	uint32_t table = wc_sb[0] + (uint32_t)rd(R_DX, 4); /* ES:EDX, in protected mode */
	switch (ah) {
	case 0x4f:
		vesa(at);
		return;
	case 0x00: { /* set mode */
		uint8_t mode = al & 0x7f;
		svga = 0;
		crtc[9] = mode == 0x13 ? 0x41 : 0x4f; /* mode 13h: every line twice */
		M[0x449] = mode;
		mwr(0x44a, 2, mode == 0x13 || mode <= 1 ? 40 : 80);
		if (mode == 0x13 && !(al & 0x80)) memset(M + 0xa0000, 0, 0x10000);
		/* the first 16 DAC entries: the default EGA colours */
		static const uint8_t ega[16][3] = {
			{ 0, 0, 0 }, { 0, 0, 42 }, { 0, 42, 0 }, { 0, 42, 42 }, { 42, 0, 0 }, { 42, 0, 42 }, { 42, 21, 0 },
			{ 42, 42, 42 }, { 21, 21, 21 }, { 21, 21, 63 }, { 21, 63, 21 }, { 21, 63, 63 }, { 63, 21, 21 },
			{ 63, 21, 63 }, { 63, 63, 21 }, { 63, 63, 63 },
		};
		memset(wc_palette, 0, sizeof wc_palette);
		memcpy(wc_palette, ega, sizeof ega);
		break;
	}
	case 0x0f: /* get mode */
		wr(R_AL, 1, M[0x449]);
		wr(R_AH, 1, M[0x44a]);
		wr(R_BH, 1, 0);
		break;
	case 0x10: /* palette */
		switch (al) {
		case 0x10: /* one DAC register: BX, DH = red, CH = green, CL = blue */
			wc_palette[bx & 0xff][0] = (uint8_t)(dx >> 8) & 0x3f;
			wc_palette[bx & 0xff][1] = (uint8_t)(cx >> 8) & 0x3f;
			wc_palette[bx & 0xff][2] = (uint8_t)cx & 0x3f;
			break;
		case 0x12: /* CX registers from BX, from the table at ES:DX */
			for (uint32_t i = 0; i < cx && bx + i < 256; i++) {
				for (int c = 0; c < 3; c++) wc_palette[bx + i][c] = M[(table + 3 * i + c) & 0xfffff] & 0x3f;
			}
			break;
		case 0x15: /* read one */
			wr(R_DH, 1, wc_palette[bx & 0xff][0]);
			wr(R_CH, 1, wc_palette[bx & 0xff][1]);
			wr(R_CL, 1, wc_palette[bx & 0xff][2]);
			break;
		case 0x17: /* read CX registers from BX into ES:DX */
			for (uint32_t i = 0; i < cx && bx + i < 256; i++) {
				for (int c = 0; c < 3; c++) mwr(table + 3 * i + c, 1, wc_palette[bx + i][c]);
			}
			break;
		}
		break;
	case 0x12:
		if ((bx & 0xff) == 0x10) { /* EGA/VGA information: colour, 256 KB */
			wr(R_BX, 2, 0x0003);
			wr(R_CX, 2, 0x0009);
		}
		break;
	case 0x1a:
		if (al == 0) { /* display combination: VGA with colour display */
			wr(R_AL, 1, 0x1a);
			wr(R_BX, 2, 0x0008);
		}
		break;
	}
	if (wc_trace) fprintf(stderr, "wc: INT 10h AX=%02x%02x BX=%04x CX=%04x DX=%04x at %05x\n", ah, al, bx, cx, dx, at);
}
