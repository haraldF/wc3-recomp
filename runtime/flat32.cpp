/* Wing Commander III: a Phar Lap 386|DOS-Extender flat-model program. The runtime is
 * its DOS extender: it loads the P3 image, gives it Phar Lap's selectors and answers the extender's
 * calls (INT 21h AX=25xxh, its segment memory calls 48h/49h/4Ah, DPMI INT 31h); other DOS calls go
 * to dos.cpp with protected-mode pointers, and interrupts reach the program's protected-mode handlers.
 * The first MB is real-mode memory as on a PC (PSP, environment, BIOS data, video); the
 * program's code and data selectors start at PROGRAM_BASE, other memory is handed out above. */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "host.h"
#include "wc_rt.h"

uint32_t wc_sb[6]; /* the segment bases */

void dos_int(uint64_t n, uint32_t at);
extern int dos_pm;
extern uint16_t wc_psp_seg;
void cpu_report(FILE *f);

/* ---- x87 helpers the generated code calls (wc_rt.h) ---- */

uint64_t x87_fint(uint64_t a, int from, int to) { return x87_fbits((double)sx(a, from), to); }
uint64_t x87_ftrunc(uint64_t a, int size) { return (uint64_t)(int64_t)x87_fval(a, size); }
uint64_t x87_fabs(uint64_t a, int s) { return x87_fbits(fabs(x87_fval(a, s)), s); }
uint64_t x87_fsqrt(uint64_t a, int s) { return x87_fbits(sqrt(x87_fval(a, s)), s); }
uint64_t x87_fceil(uint64_t a, int s) { return x87_fbits(ceil(x87_fval(a, s)), s); }
uint64_t x87_ffloor(uint64_t a, int s) { return x87_fbits(floor(x87_fval(a, s)), s); }
uint64_t x87_fround(uint64_t a, int s) { return x87_fbits(nearbyint(x87_fval(a, s)), s); }
uint64_t x87_fnan(uint64_t a, int s) { return isnan(x87_fval(a, s)); }

/* ---- descriptors ---- */

enum {
	PROGRAM_BASE = 0x400000u, /* linear address of the code and data selectors' offset 0 */
	PROGRAM_MAX = 0x2000000u, /* how far the data segment may grow (INT 21h AH=4Ah) */
	HEAP_START = PROGRAM_BASE + PROGRAM_MAX, /* other memory (AH=48h, DPMI 0501h), up to MEM_SIZE */
	/* Phar Lap's selectors */
	SEL_CODE = 0x0c, SEL_DATA = 0x14, SEL_SCREEN = 0x1c, SEL_PSP = 0x24, SEL_ENV = 0x2c, SEL_LOW = 0x34,
	PSP_LINEAR = 0x10000, ENV_LINEAR = 0xf000,
	DOS_BUFFER = 0x90000, DOS_BUFFER_SIZE = 0x4000, /* in conventional memory (9000:0000) */
	NDESC = 1024,
};

static struct { uint32_t base, limit; uint8_t used; } desc[NDESC];
static uint32_t heap_next = HEAP_START;

static void set_desc(uint16_t sel, uint32_t base, uint32_t limit)
{
	desc[sel >> 3].base = base;
	desc[sel >> 3].limit = limit;
	desc[sel >> 3].used = 1;
}

/* A free descriptor from index 16 on: its selector (RPL 3, as Phar Lap's and DPMI's are), 0 if none. */
static uint16_t new_desc(uint32_t base, uint32_t limit)
{
	for (unsigned i = 16; i < NDESC; i++) {
		if (!desc[i].used) {
			set_desc((uint16_t)(i << 3 | 7), base, limit);
			return (uint16_t)(i << 3 | 7);
		}
	}
	return 0;
}

void seg_load(int reg, uint64_t selector)
{
	uint16_t sel = (uint16_t)selector;
	unsigned i = sel >> 3;
	wr(0x100 + 2 * reg, 2, sel);
	if (sel >= 4 && (i >= NDESC || !desc[i].used)) {
		fprintf(stderr, "wc: loading an unknown selector %04x (at %05x)\n", sel, wc_last_ins);
	}
	wc_sb[reg] = i < NDESC && desc[i].used ? desc[i].base : 0;
}

uint64_t seg_limit32(uint64_t selector)
{
	unsigned i = (uint16_t)selector >> 3;
	if (i >= NDESC || !desc[i].used) return 0;
	return desc[i].limit | 1ull << 32;
}

uint64_t seg_limit(uint64_t selector)
{
	unsigned i = (uint16_t)selector >> 3;
	if (i >= NDESC || !desc[i].used) return 0;
	return (desc[i].limit & 0xffff) | 0x10000;
}

/* Memory for the program above the data segment: a bump allocator (nothing is given back). */
static uint32_t alloc_linear(uint32_t size)
{
	uint32_t a = (heap_next + 0xfff) & ~0xfffu;
	if (a + size > MEM_SIZE || a + size < a) return 0;
	heap_next = a + size;
	return a;
}

/* ---- conventional memory (AX=25C0h-25C2h): blocks between DOS_LOW and DOS_BUFFER, in paragraphs ---- */

enum { DOS_LOW = 0x2000, DOS_HIGH = DOS_BUFFER >> 4, NBLOCKS = 256 };
static struct { uint16_t seg, size; } dos_blocks[NBLOCKS]; /* sorted by segment */
static int ndos_blocks;

/* The segment of a free gap of size paragraphs, 0 if none; *largest: the largest gap */
static uint16_t dos_gap(uint32_t size, uint32_t *largest)
{
	uint32_t at = DOS_LOW;
	*largest = 0;
	for (int i = 0; i <= ndos_blocks; i++) {
		uint32_t end = i < ndos_blocks ? dos_blocks[i].seg : DOS_HIGH;
		if (end - at > *largest) *largest = end - at;
		if (end - at >= size) return (uint16_t)at;
		if (i < ndos_blocks) at = dos_blocks[i].seg + dos_blocks[i].size;
	}
	return 0;
}

static int dos_block(uint16_t seg)
{
	for (int i = 0; i < ndos_blocks; i++) {
		if (dos_blocks[i].seg == seg) return i;
	}
	return -1;
}

static void dos_alloc(uint32_t size)
{
	uint32_t largest;
	uint16_t seg = ndos_blocks < NBLOCKS ? dos_gap(size, &largest) : 0;
	if (!seg) {
		set_cf(1);
		wr(R_AX, 4, 8);
		wr(R_BX, 4, ndos_blocks < NBLOCKS ? largest : 0);
		return;
	}
	int i = 0;
	while (i < ndos_blocks && dos_blocks[i].seg < seg) i++;
	memmove(dos_blocks + i + 1, dos_blocks + i, (ndos_blocks - i) * sizeof *dos_blocks);
	dos_blocks[i].seg = seg;
	dos_blocks[i].size = (uint16_t)size;
	ndos_blocks++;
	wr(R_AX, 4, seg);
}

static void dos_free(uint16_t seg)
{
	int i = dos_block(seg);
	if (i < 0) {
		set_cf(1);
		wr(R_AX, 4, 9);
		return;
	}
	memmove(dos_blocks + i, dos_blocks + i + 1, (ndos_blocks - i - 1) * sizeof *dos_blocks);
	ndos_blocks--;
}

static void dos_resize(uint16_t seg, uint32_t size)
{
	int i = dos_block(seg);
	uint32_t end = i + 1 < ndos_blocks ? dos_blocks[i + 1].seg : DOS_HIGH;
	if (i < 0 || seg + size > end) {
		set_cf(1);
		wr(R_AX, 4, i < 0 ? 9 : 8);
		wr(R_BX, 4, i < 0 ? 0 : end - seg);
		return;
	}
	dos_blocks[i].size = (uint16_t)size;
}

/* ---- protected-mode interrupts ---- */

static struct { uint16_t sel; uint32_t off; } pm_vec[256]; /* sel 0: the runtime's own */
static struct { uint16_t sel; uint32_t off; } exc_vec[32]; /* processor exceptions' handlers */

/* Call the program's handler for vector n like the CPU does: EFLAGS, CS and EIP (of `ret`, a code
 * address) on the stack, interrupts off; its IRETD returns to here. */
static void call_handler(uint8_t n, uint32_t ret)
{
	uint32_t esp = (uint32_t)rd(R_SP, 4) - 12;
	uint64_t flags = 0;
	static const int bit[] = { 0, 2, 4, 6, 7, 8, 9, 10, 11 };
	for (int i = 0; i < 9; i++) flags |= rd(0x200 + bit[i], 1) << bit[i];
	mwr32(wc_sb[2] + esp + 8, 4, flags | 2);
	mwr32(wc_sb[2] + esp + 4, 4, rd(R_CS, 2));
	mwr32(wc_sb[2] + esp, 4, ret);
	wr(R_SP, 4, esp);
	wr(R_IF, 1, 0);
	wr(R_TF, 1, 0);
	uint16_t cs = (uint16_t)rd(R_CS, 2);
	seg_load(1, pm_vec[n].sel);
	push_ret(ret);
	run(pm_vec[n].off);
	pop_ret();
	if ((uint16_t)rd(R_CS, 2) != cs) seg_load(1, cs);
}

/* Whether the program has a handler for vector n. */
int pm_hooked(uint8_t n)
{
	return pm_vec[n].sel != 0;
}

/* A hardware interrupt (pc.cpp): to the program's handler if it has one (returns 1). */
int pm_interrupt(uint8_t n, uint32_t ret)
{
	if (!pm_vec[n].sel) return 0;
	call_handler(n, ret);
	return 1;
}

int bios_far_call(uint32_t seg_off, uint32_t at);
static void pharlap(uint32_t at);
static void dpmi(uint32_t at);

/* INT n in the program */
void flat32_int(uint64_t n, uint32_t at, uint32_t next)
{
	if (pm_vec[n].sel) {
		call_handler((uint8_t)n, next);
		return;
	}
	switch (n) {
	case 0x21:
		if ((uint8_t)rd(R_AH, 1) == 0x25 || (uint8_t)rd(R_AH, 1) == 0x48 || (uint8_t)rd(R_AH, 1) == 0x49 ||
			(uint8_t)rd(R_AH, 1) == 0x4a) {
			pharlap(at);
			return;
		}
		dos_pm = 1;
		dos_int(0x21, at);
		dos_pm = 0;
		return;
	case 0x31:
		dpmi(at);
		return;
	case 0x2f: /* MSCDEX: pointers like DOS's */
		dos_pm = 1;
		dos_int(0x2f, at);
		dos_pm = 0;
		return;
	default:
		dos_int(n, at); /* the BIOS services and the mouse: no pointers yet */
		return;
	}
}

/* ---- Phar Lap's system calls (INT 21h AX=25xxh) and segment memory ---- */

[[noreturn]] static void unknown(const char *what, unsigned fn, uint32_t at)
{
	fflush(stdout);
	fprintf(stderr, "wc: unimplemented %s %04x at %05x\n", what, fn, at);
	cpu_report(stderr);
	host_exit(2);
}

/* AX=2511h: the interrupt with the real-mode segment registers and EAX/EDX from the block (+0
 * interrupt, +2 DS, +4 ES, +6 FS, +8 GS, +0Ah EAX, +0Eh EDX), the other registers as they are;
 * the results go back into the block and EAX. */
int dos_real; /* a real-mode interrupt (2511h) is being done: segment registers hold segments */

static void real_interrupt(uint32_t b, uint32_t at)
{
	uint16_t n = (uint16_t)mrd32(b, 2);
	if (wc_trace) {
		fprintf(stderr, "wc: real-mode INT %02xh DS=%04llx ES=%04llx EAX=%08llx EDX=%08llx EBX=%08llx ECX=%08llx\n", n,
			mrd32(b + 2, 2), mrd32(b + 4, 2), mrd32(b + 10, 4), mrd32(b + 14, 4), rd(R_BX, 4), rd(R_CX, 4));
	}
	uint16_t sel[6];
	for (int r = 0; r < 6; r++) sel[r] = (uint16_t)rd(0x100 + 2 * r, 2);
	uint64_t edx = rd(R_DX, 4);
	wr(R_DS, 2, mrd32(b + 2, 2));
	wr(R_ES, 2, mrd32(b + 4, 2));
	wr(R_FS, 2, mrd32(b + 6, 2));
	wr(R_GS, 2, mrd32(b + 8, 2));
	wr(R_AX, 4, mrd32(b + 10, 4));
	wr(R_DX, 4, mrd32(b + 14, 4));
	dos_real = 1;
	dos_int(n, at);
	dos_real = 0;
	mwr32(b + 2, 2, rd(R_DS, 2));
	mwr32(b + 4, 2, rd(R_ES, 2));
	mwr32(b + 6, 2, rd(R_FS, 2));
	mwr32(b + 8, 2, rd(R_GS, 2));
	mwr32(b + 10, 4, rd(R_AX, 4));
	mwr32(b + 14, 4, rd(R_DX, 4));
	for (int r = 0; r < 6; r++) wr(0x100 + 2 * r, 2, sel[r]); /* the bases in wc_sb[] never changed */
	wr(R_DX, 4, edx);
}

static void pharlap(uint32_t at)
{
	uint16_t ax = (uint16_t)rd(R_AX, 2);
	uint8_t ah = (uint8_t)(ax >> 8), al = (uint8_t)ax, cl = (uint8_t)rd(R_CL, 1);
	uint32_t ds = wc_sb[3], edx = (uint32_t)rd(R_DX, 4), ebx = (uint32_t)rd(R_BX, 4);
	if (wc_trace) {
		fprintf(stderr, "wc: Phar Lap AX=%04x EBX=%08x ECX=%08llx EDX=%08x at %05x\n", ax, ebx, rd(R_CX, 4), edx, at);
	}
	set_cf(0);
	if (ah == 0x4a) { /* resize segment ES to EBX pages: the data segment grows in place */
		uint16_t es = (uint16_t)rd(R_ES, 2);
		uint32_t size = ebx * 4096;
		if ((es >> 3) == (SEL_DATA >> 3) && size <= PROGRAM_MAX) {
			desc[SEL_DATA >> 3].limit = desc[SEL_CODE >> 3].limit = size - 1;
			return;
		}
		set_cf(1);
		wr(R_AX, 4, 8);
		wr(R_BX, 4, (PROGRAM_MAX - 0) / 4096);
		return;
	}
	if (ah == 0x48) { /* allocate a segment of EBX pages */
		uint32_t size = ebx * 4096, a = alloc_linear(size);
		uint16_t sel = a ? new_desc(a, size - 1) : 0;
		if (!sel) {
			set_cf(1);
			wr(R_AX, 4, 8);
			wr(R_BX, 4, (MEM_SIZE - heap_next) / 4096);
			return;
		}
		memset(M + a, 0, size);
		wr(R_AX, 4, sel);
		return;
	}
	if (ah == 0x49) { /* free segment ES */
		uint16_t es = (uint16_t)rd(R_ES, 2);
		if ((es >> 3) >= 16 && (es >> 3) < NDESC) desc[es >> 3].used = 0;
		return;
	}
	switch (al) {
	case 0x02: /* get the protected-mode vector CL: ES:EBX */
		seg_load(0, pm_vec[cl].sel ? pm_vec[cl].sel : SEL_CODE);
		wr(R_BX, 4, pm_vec[cl].sel ? pm_vec[cl].off : 0);
		break;
	case 0x03: /* get the real-mode vector CL: EBX = segment:offset */
		wr(R_BX, 4, mrd(cl * 4, 4));
		break;
	case 0x04: /* set the protected-mode vector CL to DS:EDX */
	case 0x06: /* the same, and real-mode interrupts too (always gain control in protected mode) */
		pm_vec[cl].sel = (uint16_t)rd(R_DS, 2);
		pm_vec[cl].off = edx;
		break;
	case 0x05: /* set the real-mode vector CL to EBX: nothing runs in real mode here */
		mwr(cl * 4, 4, ebx);
		break;
	case 0x07: /* set both: protected mode DS:EDX, real mode EBX */
		pm_vec[cl].sel = (uint16_t)rd(R_DS, 2);
		pm_vec[cl].off = edx;
		mwr(cl * 4, 4, ebx);
		break;
	case 0x0a: { /* map ECX pages of physical memory at EBX to the end of segment ES: EAX = its offset.
		* Physical is linear here: an empty segment moves there, others reach it by wrapping around */
		uint16_t es = (uint16_t)rd(R_ES, 2);
		unsigned i = es >> 3;
		if (i >= NDESC || !desc[i].used) {
			set_cf(1);
			wr(R_AX, 4, 9);
			break;
		}
		uint32_t size = (uint32_t)rd(R_CX, 4) * 4096;
		if (desc[i].limit == 0xffffffffu) { /* empty (allocated with 0 pages) */
			desc[i].base = ebx;
			desc[i].limit = size - 1;
			wr(R_AX, 4, 0);
		}
		else {
			wr(R_AX, 4, ebx - desc[i].base);
			desc[i].limit = 0xffffffffu;
		}
		for (int r = 0; r < 6; r++) {
			if ((uint16_t)rd(0x100 + 2 * r, 2) == es) seg_load(r, es);
		}
		if (wc_trace) fprintf(stderr, "wc: mapped at %04x:%08llx (base %08x)\n", es, rd(R_AX, 4), desc[i].base);
		break;
	}
	case 0x10: { /* call the real-mode procedure at EBX with the registers in the block at DS:EDX (DS, ES,
		* FS, GS words, then EAX, EBX, ECX, EDX): our BIOS's entries only (VESA480.DLL's bank switch).
		* The procedure's registers come back in the registers and the block. */
		uint32_t b = ds + edx;
		static const uint32_t regs[4] = { R_AX, R_BX, R_CX, R_DX };
		for (int r = 0; r < 4; r++) wr(regs[r], 4, mrd32(b + 8 + 4 * r, 4));
		if (!bios_far_call(ebx, at)) unknown("real-mode procedure call to", ebx >> 16, at);
		for (int r = 0; r < 4; r++) mwr32(b + 8 + 4 * r, 4, rd(regs[r], 4));
		break;
	}
	case 0x0e: /* call the real-mode procedure at EBX (segment:offset): our BIOS's entries only */
		if (!bios_far_call(ebx, at)) unknown("real-mode procedure call to", ebx >> 16, at);
		break;
	case 0x11: /* issue a real-mode interrupt: DS:EDX a parameter block */
		real_interrupt(ds + edx, at);
		break;
	case 0x17: /* the DOS data buffer (for real-mode calls): ES:EBX, real-mode address ECX, size EDX */
		seg_load(0, SEL_LOW);
		wr(R_BX, 4, DOS_BUFFER);
		wr(R_CX, 4, DOS_BUFFER << 12);
		wr(R_DX, 4, DOS_BUFFER_SIZE);
		break;
	case 0x31: { /* read (ECX 0) or write (1) the descriptor of selector BX at DS:EDX (8 bytes) */
		unsigned i = (uint16_t)ebx >> 3;
		uint8_t *d = M + ds + edx;
		if (i >= NDESC) {
			set_cf(1);
			break;
		}
		if (rd(R_CX, 4) == 0) {
			uint32_t base = desc[i].base, limit = desc[i].limit, g = limit > 0xfffff;
			if (g) limit >>= 12;
			uint8_t bytes[8] = { (uint8_t)limit, (uint8_t)(limit >> 8), (uint8_t)base, (uint8_t)(base >> 8),
				(uint8_t)(base >> 16), 0xf2, (uint8_t)((limit >> 16 & 15) | 0x40 | g << 7), (uint8_t)(base >> 24) };
			memcpy(d, bytes, 8);
			break;
		}
		uint32_t limit = d[0] | d[1] << 8 | (d[6] & 15) << 16;
		desc[i].base = d[2] | d[3] << 8 | d[4] << 16 | (uint32_t)d[7] << 24;
		desc[i].limit = d[6] & 0x80 ? limit << 12 | 0xfff : limit;
		desc[i].used = 1;
		for (int r = 0; r < 6; r++) {
			if ((uint16_t)rd(0x100 + 2 * r, 2) >> 3 == i) seg_load(r, rd(0x100 + 2 * r, 2));
		}
		break;
	}
	case 0x32: /* get the handler of processor exception CL: ES:EBX */
		seg_load(0, exc_vec[cl & 31].sel ? exc_vec[cl & 31].sel : SEL_CODE);
		wr(R_BX, 4, exc_vec[cl & 31].off);
		break;
	case 0x33: /* set the handler of processor exception CL to DS:EDX (no exceptions happen here) */
		exc_vec[cl & 31].sel = (uint16_t)rd(R_DS, 2);
		exc_vec[cl & 31].off = edx;
		break;
	case 0xc0: /* allocate BX paragraphs of conventional memory: AX = the segment */
		dos_alloc((uint16_t)ebx);
		break;
	case 0xc1: /* free the conventional memory block at segment CX */
		dos_free((uint16_t)rd(R_CX, 2));
		break;
	case 0xc2: /* resize the block at CX to BX paragraphs */
		dos_resize((uint16_t)rd(R_CX, 2), (uint16_t)ebx);
		break;
	case 0x20: { /* memory statistics into DS:EDX (100 bytes): +0Ch the 4 KB pages the program may have
		* (the C library's startup takes the program's memory, PSP +5Ch, from them) */
		memset(M + ds + edx, 0, 100);
		uint32_t pages = (PROGRAM_MAX + MEM_SIZE - heap_next) / 4096; /* the program's and what is left */
		memcpy(M + ds + edx + 0x0c, &pages, 4);
		break;
	}
	case 0x26: { /* configuration into DS:EBX: the PSP, environment and first-MB selectors at +104h..
		* (+0D8h zero: no real-mode transfer buffer needed) */
		uint32_t b = ds + ebx;
		memset(M + b, 0, 0x180);
		uint16_t sels[] = { SEL_PSP, SEL_ENV, SEL_LOW };
		memcpy(M + b + 0x104, sels, sizeof sels);
		break;
	}
	case 0x2b: /* page locking (BH 5/6: lock/unlock, 9/10 for DOS memory): memory never pages out here */
		break;
	default:
		unknown("Phar Lap call INT 21h AX=", ax, at);
	}
}

/* ---- DPMI (INT 31h) ---- */

static void dpmi(uint32_t at)
{
	uint16_t ax = (uint16_t)rd(R_AX, 2), bx = (uint16_t)rd(R_BX, 2);
	if (wc_trace) {
		fprintf(stderr, "wc: DPMI AX=%04x BX=%04x CX=%04llx DX=%04llx at %05x\n", ax, bx, rd(R_CX, 2), rd(R_DX, 2), at);
	}
	set_cf(0);
	switch (ax) {
	case 0x0006: { /* segment base of BX: CX:DX */
		uint32_t base = (bx >> 3) < NDESC ? desc[bx >> 3].base : 0;
		wr(R_CX, 2, base >> 16);
		wr(R_DX, 2, base & 0xffff);
		break;
	}
	case 0x0007: /* set the base of BX to CX:DX */
		if ((bx >> 3) < NDESC) desc[bx >> 3].base = (uint32_t)(rd(R_CX, 2) << 16 | rd(R_DX, 2));
		for (int r = 0; r < 6; r++) {
			if ((uint16_t)rd(0x100 + 2 * r, 2) == bx) seg_load(r, bx);
		}
		break;
	case 0x0008: /* set the limit of BX to CX:DX */
		if ((bx >> 3) < NDESC) desc[bx >> 3].limit = (uint32_t)(rd(R_CX, 2) << 16 | rd(R_DX, 2));
		break;
	default:
		unknown("DPMI call INT 31h AX=", ax, at);
	}
}

/* ---- the process ---- */

/* Load image.bin (the unpacked P3 load image) and set up the program as Phar Lap starts it. */
void flat32_start(const char *dir, int argc, char **argv, void (*set_command_line)(uint8_t *, int, char **))
{
	char path[4096];
	snprintf(path, sizeof path, "%s/image.bin", dir);
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		exit(1);
	}
	size_t size = fread(M + PROGRAM_BASE + wc_program.flat_base, 1, PROGRAM_MAX, f);
	fclose(f);
	wc_image_lo = PROGRAM_BASE + wc_program.flat_base;
	wc_image_hi = wc_image_lo + (uint32_t)size;

	/* the PSP (with Phar Lap's +5Ch: the program's memory in bytes) and the environment */
	static const char vars[] = "COMSPEC=C:\\COMMAND.COM\0\0\1\0C:\\";
	memcpy(M + ENV_LINEAR, vars, sizeof vars - 1);
	strcpy((char *)M + ENV_LINEAR + sizeof vars - 1, wc_program.exe);
	uint8_t *psp = M + PSP_LINEAR;
	psp[0] = 0xcd;
	psp[1] = 0x20;
	psp[2] = 0x00;
	psp[3] = 0xa0;
	psp[0x2c] = SEL_ENV & 0xff;
	psp[0x2d] = SEL_ENV >> 8;
	/* the header asks for at least flat_size and at most everything (maximum extra memory FFFFFFFFh):
	 * 16 MB, as on a PC with that much. The C library makes its heap there (with only the minimum,
	 * the game said it lacked "high memory": it wants 4.3 MB of heap) and then grows the segment by
	 * ~4.4 MB more, so not all of PROGRAM_MAX */
	uint32_t need = PROGRAM_MAX / 2;
	memcpy(psp + 0x5c, &need, 4);
	set_command_line(psp, argc, argv);
	wc_psp_seg = PSP_LINEAR >> 4;

	set_desc(SEL_CODE, PROGRAM_BASE, need - 1);
	set_desc(SEL_DATA, PROGRAM_BASE, need - 1);
	set_desc(SEL_SCREEN, 0xa0000, 0x1ffff);
	set_desc(SEL_PSP, PSP_LINEAR, 0xff);
	set_desc(SEL_ENV, ENV_LINEAR, 0xfff);
	set_desc(SEL_LOW, 0, 0xfffff);

	wc_code_base = PROGRAM_BASE;
	seg_load(1, SEL_CODE);
	seg_load(3, SEL_DATA | 3);
	seg_load(2, SEL_DATA | 3);
	seg_load(0, SEL_PSP);
	seg_load(4, 0);
	seg_load(5, 0);
	wr(R_SP, 4, wc_program.flat_esp);
	wr(R_IF, 1, 1);
}
