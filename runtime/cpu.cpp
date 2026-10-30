/* CPU state, dispatch into the recompiled code, failure reporting and I/O ports. */
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "host.h"
#include "wc_rt.h"

uint8_t R[0x400];
uint8_t M[MEM_SIZE + 16];
uint64_t wc_icount;
int wc_trace;
uint32_t wc_watch, wc_last_ins;
uint32_t wc_ret_stack[WC_RET_STACK];
int wc_ret_depth;
uint32_t wc_image_lo, wc_image_hi;
uint64_t wc_video_writes;


static int find_code(uint32_t linear)
{
	uint32_t lo = 0, hi = wc_code_count;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		if (wc_code_addr[mid] < linear) {
			lo = mid + 1;
		}
		else {
			hi = mid;
		}
	}
	return lo < wc_code_count && wc_code_addr[lo] == linear ? (int)lo : -1;
}

/* WC_DUMP=file: the whole memory, for looking at the program's state afterwards */
void wc_dump_memory(const char *why)
{
	const char *path = getenv("WC_DUMP");
	FILE *f = path ? fopen(path, "wb") : NULL;
	if (!f) return;
	fwrite(M, 1, MEM_SIZE, f);
	fclose(f);
	fprintf(stderr, "wc: memory saved to %s (%s)\n", path, why);
}

uint32_t wc_code_base;
int wc_unwind = -1;

void ret_to(uint32_t code)
{
	if (wc_ret_depth > 0 && wc_ret_stack[wc_ret_depth - 1] == code) return;
	for (int i = wc_ret_depth - 2; i >= 0; i--) {
		if (wc_ret_stack[i] == code) {
			if (wc_trace) fprintf(stderr, "wc: return to %05x unwinds %d calls\n", code, wc_ret_depth - i - 1);
			wc_unwind = i;
			return;
		}
	}
	if (wc_trace) fprintf(stderr, "wc: return to %05x, which no call expects (at %05x)\n", code, wc_last_ins);
}

/* ---- drivers loaded at runtime ---- */

/* Whether the image's instruction bytes are in memory at base */
static bool matches(uint32_t base, const wc_driver *d)
{
	for (uint32_t r = 0; r < d->nranges; r++) {
		uint32_t off = d->ranges[r][0], len = d->ranges[r][1];
		if (base + off + len > MEM_SIZE || memcmp(M + base + off, d->image + off, len)) return false;
	}
	return true;
}

/* A quick check of a driver found before: the first bytes of its instructions */
static bool still_there(uint32_t base, const wc_driver *d)
{
	return d->nranges && base + d->ranges[0][0] + 16 <= MEM_SIZE &&
		!memcmp(M + base + d->ranges[0][0], d->image + d->ranges[0][0], std::min(d->ranges[0][1], 16u));
}

static bool is_flat(const wc_driver *d)
{
	return d->virt < DRIVER_VIRT;
}

static int find_in(const uint32_t *addr, uint32_t count, uint32_t code);

/* Whether the code at offset off is what a driver's capture had there (16 bytes, or up to its end) */
static bool same_code(uint32_t base, const wc_driver *d, uint32_t off)
{
	uint32_t n = off < d->size ? std::min(d->size - off, 16u) : 0;
	return n && base + off + n <= MEM_SIZE && !memcmp(M + base + off, d->image + off, n);
}

/* The driver with its own code selector at that selector's base, for a call to offset off, if any.
 * The same memory can hold different code over time (the game loads one driver, then another
 * there), so of the drivers whose instructions are there, the one that has an instruction at off
 * wins; else one whose capture has the bytes that are there now (its code at off isn't
 * recompiled yet: "missing code"). Otherwise it is new code. */
static const wc_driver *driver_at(uint32_t base, uint32_t off)
{
	static uint32_t cached_base;
	static const wc_driver *cached;
	if (cached && cached_base == base && still_there(base, cached) &&
		(off == UINT32_MAX || find_in(cached->addr, cached->count, cached->virt + off) >= 0)) {
		return cached;
	}
	const wc_driver *fallback = nullptr;
	for (uint32_t i = 0; i < wc_driver_count; i++) {
		const wc_driver *d = wc_drivers[i];
		if (is_flat(d) || !matches(base, d)) continue;
		if (off == UINT32_MAX || find_in(d->addr, d->count, d->virt + off) >= 0) {
			if (wc_trace && d != cached) fprintf(stderr, "wc: driver %s at %08x\n", d->name, base);
			cached_base = base;
			return cached = d;
		}
		if (!fallback && same_code(base, d, off)) fallback = d;
	}
	return fallback;
}

/* The flat driver whose image holds an offset of the program's segment, if it is in memory */
static const wc_driver *flat_driver_at(uint32_t off)
{
	static const wc_driver *cached;
	if (cached && off - cached->virt < cached->size && still_there(wc_code_base + cached->virt, cached)) return cached;
	for (uint32_t i = 0; i < wc_driver_count; i++) {
		const wc_driver *d = wc_drivers[i];
		if (is_flat(d) && off - d->virt < d->size && matches(wc_code_base + d->virt, d)) {
			if (wc_trace) fprintf(stderr, "wc: driver %s at offset %08x\n", d->name, d->virt);
			return cached = d;
		}
	}
	return nullptr;
}

/* The driver a code address belongs to, if any */
static const wc_driver *driver_code(uint32_t code)
{
	if (code < wc_program_size) return nullptr;
	if (code < DRIVER_VIRT) return flat_driver_at(code);
	for (uint32_t i = 0; i < wc_driver_count; i++) {
		if (!is_flat(wc_drivers[i]) && code - wc_drivers[i]->virt < DRIVER_SPACING) return wc_drivers[i];
	}
	return nullptr;
}

/* Code address of an offset reached with the current CS: the program's offsets (and its flat
 * drivers'), or a driver's virtual address. Return addresses that recompiled driver code pushed
 * are virtual already. */
uint32_t code_addr(uint64_t linear)
{
	if (wc_sb[1] == wc_code_base || linear >= DRIVER_VIRT) return (uint32_t)linear;
	const wc_driver *d = driver_at(wc_sb[1], (uint32_t)linear);
	return d ? d->virt + (uint32_t)linear : (uint32_t)linear;
}

/* A call into code the program loaded, which isn't recompiled: save it for "npm run discover" and
 * stop with exit code 5. Through a selector of its own: from the base to the limit (at most 1 MB);
 * in the program's segment: 128 KB around the offset. */
[[noreturn]] static void unknown_code(uint32_t offset)
{
	uint16_t cs = (uint16_t)rd(R_CS, 2);
	bool flat = wc_sb[1] == wc_code_base;
	uint32_t from = 0, size = (uint32_t)seg_limit32(cs) + 1;
	if (flat) {
		from = std::max((offset & ~0xfffu) - 0x10000, (wc_program_size + 0xfff) & ~0xfffu);
		size = (offset & ~0xfffu) + 0x10000 - from;
	}
	else if (size == 0 || size > 0x100000) {
		size = 0x100000;
	}
	uint32_t base = wc_sb[1] + from;
	if (base + size > MEM_SIZE) size = MEM_SIZE - base;
	fflush(stdout);
	fprintf(stderr, "wc: call into code loaded at runtime at %04x:%08x (%s, captured from offset %08x, %x bytes), "
		"which is not recompiled\n", cs, offset, flat ? "flat" : "own segment", from, size);
	if (const char *dir = getenv("WC_LOADED_DIR")) {
		char path[1024];
		snprintf(path, sizeof path, "%s/%08x-%08x.bin", dir, base, offset);
		if (FILE *f = fopen(path, "wb")) {
			fwrite(M + base, 1, size, f);
			fclose(f);
			fprintf(stderr, "wc: saved loaded code to %s\n", path);
		}
	}
	cpu_report(stderr);
	host_exit(5);
}

void run(uint32_t linear)
{
	if (wc_sb[1] != wc_code_base) {
		if (linear >= DRIVER_VIRT) { /* a return address the recompiled driver pushed */
			run_code((uint32_t)linear);
			return;
		}
		const wc_driver *d = driver_at(wc_sb[1], (uint32_t)linear);
		if (!d) unknown_code((uint32_t)linear);
		run_code(d->virt + (uint32_t)linear);
		return;
	}
	if (find_code((uint32_t)linear) < 0) {
		/* The C library's int86() calls "INT n; RET" that it wrote on the stack: do that here */
		uint32_t a = wc_sb[1] + (uint32_t)linear;
		if (a < MEM_SIZE - 3 && M[a] == 0xcd && M[a + 2] == 0xc3) {
			cpu_int(M[a + 1], (uint32_t)linear, (uint32_t)linear + 2);
			wr(R_SP, 4, rd(R_SP, 4) + 4); /* the RET: the caller continues after its call */
			return;
		}
		/* code loaded into the program's memory */
		if (linear >= wc_program_size && linear < DRIVER_VIRT && !flat_driver_at((uint32_t)linear)) {
			unknown_code((uint32_t)linear);
		}
	}
	run_code((uint32_t)linear);
}

static int find_in(const uint32_t *addr, uint32_t count, uint32_t code)
{
	uint32_t lo = 0, hi = count;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		if (addr[mid] < code) lo = mid + 1;
		else hi = mid;
	}
	return lo < count && addr[lo] == code ? (int)lo : -1;
}

void run_code(uint32_t code)
{
	if (const wc_driver *d = driver_code(code)) {
		int k = find_in(d->addr, d->count, code);
		if (k >= 0) {
			d->func[k](code);
			return;
		}
	}
	else if (int i = find_code(code); i >= 0) {
		wc_code_func[i](code);
		return;
	}
	/* Exit code 4 + this line: "npm run discover" adds the address as an entry point. */
	fflush(stdout);
	fprintf(stderr, "wc: missing code at %05x\n", code);
	cpu_report(stderr);
	host_exit(4);
}

static void dump_regs(FILE *f)
{
	fprintf(f, "  EAX=%08llx EBX=%08llx ECX=%08llx EDX=%08llx ESI=%08llx EDI=%08llx EBP=%08llx ESP=%08llx\n",
		rd(R_AX, 4), rd(R_BX, 4), rd(R_CX, 4), rd(R_DX, 4), rd(R_SI, 4), rd(R_DI, 4), rd(R_BP, 4), rd(R_SP, 4));
	fprintf(f, "  CS=%04llx DS=%04llx ES=%04llx SS=%04llx FS=%04llx GS=%04llx  CF=%llu ZF=%llu SF=%llu OF=%llu IF=%llu\n",
		rd(R_CS, 2), rd(R_DS, 2), rd(R_ES, 2), rd(R_SS, 2), rd(R_FS, 2), rd(R_GS, 2), rd(R_CF, 1), rd(R_ZF, 1),
		rd(R_SF, 1), rd(R_OF, 1), rd(R_IF, 1));
}

void pc_report(FILE *f);

extern uint8_t wc_palette[256][3];

/* WC_SCREENSHOT=file: the VGA palette (768 bytes, 6-bit) and the 320x200 frame buffer, raw. */
static void screenshot(void)
{
	const char *path = getenv("WC_SCREENSHOT");
	FILE *f = path ? fopen(path, "wb") : NULL;
	if (f) {
		fwrite(wc_palette, 1, sizeof wc_palette, f);
		fwrite(M + 0xa0000, 1, 320 * 200, f);
		fclose(f);
	}
}

static void print_callers(FILE *f)
{
	fprintf(f, "  called from (innermost first, code addresses after each call):");
	for (int i = wc_ret_depth - 1; i >= 0 && i >= wc_ret_depth - 8; i--) {
		fprintf(f, " %05x", wc_ret_stack[i]);
	}
	fprintf(f, "\n");
}

void cpu_report(FILE *f)
{
	screenshot();
	pc_report(f);
	fprintf(f, "  %llu instructions executed, %llu writes to video memory\n", (unsigned long long)wc_icount,
		(unsigned long long)wc_video_writes);
	dump_regs(f);
	print_callers(f);
}

[[noreturn]] void cpu_fail(const char *what, uint32_t at)
{
	fflush(stdout);
	fprintf(stderr, "wc: %s %05x (CS=%04llx)\n", what, at, rd(R_CS, 2));
	cpu_report(stderr);
	host_exit(3);
}

[[noreturn]] void cpu_div0(uint32_t at)
{
	cpu_fail("divide error at", at);
}

void trace_ins(uint32_t at)
{
	fprintf(stderr, "  %05x  EAX=%08llx EBX=%08llx ECX=%08llx EDX=%08llx ESP=%08llx EBP=%08llx DS=%04llx ES=%04llx SS=%04llx IF=%llu\n",
		at, rd(R_AX, 4), rd(R_BX, 4), rd(R_CX, 4), rd(R_DX, 4), rd(R_SP, 4), rd(R_BP, 4), rd(R_DS, 2), rd(R_ES, 2),
		rd(R_SS, 2), rd(R_IF, 1));
}

void watch_hit(uint32_t addr, int size, uint64_t value)
{
	fprintf(stderr, "wc: write %0*llx to %05x (watch %05x) by instruction %05x, CS=%04llx\n", size * 2,
		(unsigned long long)value, addr, wc_watch, wc_last_ins, rd(R_CS, 2));
	print_callers(stderr);
}

/* Writes to A0000h..BFFFFh (video memory). */
void mem_write_hook(uint32_t addr, int size)
{
	if (addr < 0xb0000) {
		wc_video_writes++;
	}
	(void)size;
}
