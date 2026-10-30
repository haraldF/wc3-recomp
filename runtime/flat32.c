/* Wing Commander III: a Phar Lap 386|DOS-Extender flat-model program. The runtime is
 * its DOS extender: it loads the P3 image, gives it Phar Lap's selectors and answers the extender's
 * calls (INT 21h AX=25xxh, its segment memory calls 48h/49h/4Ah, DPMI INT 31h); other DOS calls go
 * to dos.c with protected-mode pointers, and interrupts reach the program's protected-mode handlers.
 * The first MB is real-mode memory as on a PC (PSP, environment, BIOS data, video); the
 * program's code and data selectors start at PROGRAM_BASE, other memory is handed out above. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ---- protected-mode interrupts ---- */

static struct { uint16_t sel; uint32_t off; } pm_vec[256]; /* sel 0: the runtime's own */

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

/* A hardware interrupt (pc.c): to the program's handler if it has one (returns 1). */
int pm_interrupt(uint8_t n, uint32_t ret)
{
	if (!pm_vec[n].sel) return 0;
	call_handler(n, ret);
	return 1;
}

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
	default:
		dos_int(n, at); /* the BIOS services and the mouse: no pointers yet */
		return;
	}
}

/* ---- Phar Lap's system calls (INT 21h AX=25xxh) and segment memory ---- */

static _Noreturn void unknown(const char *what, unsigned fn, uint32_t at)
{
	fflush(stdout);
	fprintf(stderr, "wc: unimplemented %s %04x at %05x\n", what, fn, at);
	cpu_report(stderr);
	host_exit(2);
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
	case 0x20: { /* memory statistics into DS:EDX (100 bytes): +0Ch the free 4 KB pages */
		memset(M + ds + edx, 0, 100);
		uint32_t pages = (MEM_SIZE - heap_next) / 4096;
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
	uint32_t need = wc_program.flat_size;
	memcpy(psp + 0x5c, &need, 4);
	set_command_line(psp, argc, argv);
	wc_psp_seg = PSP_LINEAR >> 4;

	set_desc(SEL_CODE, PROGRAM_BASE, need - 1);
	set_desc(SEL_DATA, PROGRAM_BASE, need - 1);
	set_desc(SEL_SCREEN, 0xa0000, 0x1ffff);
	set_desc(SEL_PSP, PSP_LINEAR, 0xff);
	set_desc(SEL_ENV, ENV_LINEAR, 0xfff);
	set_desc(SEL_LOW, 0, 0xfffff);

	seg_load(1, SEL_CODE);
	seg_load(3, SEL_DATA | 3);
	seg_load(2, SEL_DATA | 3);
	seg_load(0, SEL_PSP);
	seg_load(4, 0);
	seg_load(5, 0);
	wr(R_SP, 4, wc_program.flat_esp);
	wr(R_IF, 1, 1);
}
