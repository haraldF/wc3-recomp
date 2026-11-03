/* The mouse driver (INT 33h), fed from the host's window. Positions are in the driver's virtual
 * screen, which in mode 13h is 640x200 (x doubled), like Microsoft's driver; the window covers it,
 * or the last range the program has set from 0 (Wing Commander III sets 640x480). */
#include <atomic>
#include <cstdio>

#include "host.h"
#include "wc_rt.h"

/* Events from the host, on its thread: a single-producer, single-consumer ring. */
static struct { int16_t x, y; uint8_t buttons, relative; uint32_t warp; } events[256];
static std::atomic<uint32_t> events_head, events_tail;
static std::atomic<int> mouse_seen; /* the host had a mouse event: the driver reports a mouse */

/* In the window the pointer is where the Mac's is over the picture. When the program puts it
 * somewhere (AX=4), or changes the range so that the same place means another position, a real
 * driver's pointer stays at its numbers: the host moves the Mac's pointer there instead
 * (pc_mouse_warp). Without that, flight (range 0..1278) began with the pointer where "Fly MISSION."
 * was clicked, at the right edge, and the ship turned. Positions the host sent before it moved the
 * pointer are stale then. */
static std::atomic<uint32_t> warp_wanted, warp_done; /* requests made, and taken by the host */
static std::atomic<int> warp_x, warp_y;               /* where to, in the window's 320x200 */
static std::atomic<int> host_warps;                   /* the host moves its pointer */

static void post(int x, int y, int buttons, int relative, uint32_t warp)
{
	uint32_t tail = events_tail.load(std::memory_order_relaxed);
	if (tail - events_head.load(std::memory_order_acquire) < 256) {
		events[tail % 256].x = (int16_t)x;
		events[tail % 256].y = (int16_t)y;
		events[tail % 256].buttons = (uint8_t)buttons;
		events[tail % 256].relative = (uint8_t)relative;
		events[tail % 256].warp = warp;
		events_tail.store(tail + 1, std::memory_order_release);
	}
	mouse_seen.store(1);
}

void pc_warp_end(void);
static uint32_t host_saw(void) { return warp_done.load(std::memory_order_relaxed); }
void pc_mouse(int x, int y, int buttons) { if (buttons) pc_warp_end(); post(x, y, buttons, 0, host_saw()); }
/* WC_KEYS, on the emulation's thread: never stale, and no warp end */
void mouse_post(int x, int y, int buttons) { post(x, y, buttons, 0, warp_wanted.load(std::memory_order_relaxed)); }
void pc_mouse_move(int dx, int dy, int buttons) { if (buttons) pc_warp_end(); post(dx, dy, buttons, 1, host_saw()); }

int pc_mouse_warp(int *x, int *y)
{
	host_warps.store(1);
	uint32_t wanted = warp_wanted.load(std::memory_order_acquire);
	if (wanted == warp_done.load(std::memory_order_relaxed)) return 0;
	*x = warp_x.load();
	*y = warp_y.load();
	warp_done.store(wanted, std::memory_order_release);
	return 1;
}

static int x, y = 100, min_x, max_x = 639, min_y, max_y = 199;
/* What the window's 320x200 covers: the last range set from 0 (WC3 sets 0..1278 in flight, 0..640
 * in the options screen); a range starting further in only confines the pointer. */
static int virt_w = 640, virt_h = 200;
static uint8_t buttons;     /* held now */
static uint8_t clicked;     /* pressed since the program last read the buttons (so short clicks count) */
/* A click shorter than this (a trackpad tap, a browser's synthetic click) is reported as held this
 * long: the barracks, for one, misses a button that is already up at its next poll. */
enum { MIN_CLICK_NS = 60000000 };
static uint64_t press_ns[3];
static uint8_t short_click; /* released before MIN_CLICK_NS: still reported as held */
uint64_t pc_now_ns(void);

static uint8_t reported_buttons(void)
{
	uint8_t b = buttons | clicked;
	if (short_click) {
		uint64_t now = pc_now_ns();
		for (int i = 0; i < 3; i++) {
			if (!(short_click & (1 << i))) continue;
			if (now - press_ns[i] < MIN_CLICK_NS) b |= (uint8_t)(1 << i);
			else short_click &= (uint8_t)~(1 << i);
		}
	}
	return b;
}
static uint16_t presses[3], releases[3];
static int16_t press_x[3], press_y[3], release_x[3], release_y[3];
static int mickey_x, mickey_y; /* motion counters since the last read (8 per 8 pixels) */
static int visible = -1;     /* cursor shown when >= 0; this driver draws none (the game draws its own) */

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* The event handler (AX=0Ch): real-mode code at seg:off, called for the events in its mask (bit 0
 * movement, 1/2 left pressed/released, 3/4 right, 5/6 middle). Nothing else runs in real mode here,
 * so a small interpreter runs it: Wing Commander III's only stores BX, CX and DX in its segment for
 * the game to read. It is called as the state the program would see changes (mouse_poll), with
 * short clicks held as long as for AX=3, since the game only looks at the last state. */
static uint16_t handler_seg, handler_off, handler_mask;
static int handler_x, handler_y, handler_buttons = -1; /* what the handler was last told */

static int real_call(uint16_t cs, uint16_t ip, uint16_t r[8]) /* AX CX DX BX SP BP SI DI; 0 if not run */
{
	uint16_t stack[64];
	unsigned sp = 64;
	uint16_t ds = cs, es = cs;
	for (int steps = 0; steps < 1000; steps++) {
		uint16_t seg = ds;
		uint8_t op;
		for (;;) { /* prefixes */
			op = M[cs * 16u + ip++];
			if (op == 0x2e) seg = cs;
			else if (op == 0x26) seg = es;
			else if (op == 0x36 || op == 0x3e) seg = ds; /* SS is ours: the same as DS here */
			else break;
		}
		if (op >= 0x50 && op <= 0x57) {
			if (!sp) return 0;
			stack[--sp] = r[op & 7];
			continue;
		}
		if (op >= 0x58 && op <= 0x5f) {
			if (sp == 64) return 0;
			r[op & 7] = stack[sp++];
			continue;
		}
		if (op >= 0xb8 && op <= 0xbf) {
			r[op & 7] = (uint16_t)(M[cs * 16u + ip] | M[cs * 16u + ip + 1] << 8);
			ip += 2;
			continue;
		}
		if (op == 0xcb) return 1; /* RETF: back to the driver */
		if (op != 0x89 && op != 0x8b && op != 0x31 && op != 0x33) break;
		uint8_t modrm = M[cs * 16u + ip++];
		int mod = modrm >> 6, reg = modrm >> 3 & 7, rm = modrm & 7;
		if (mod == 3) {
			uint16_t &dst = op & 2 ? r[reg] : r[rm], src = op & 2 ? r[rm] : r[reg];
			dst = op == 0x89 || op == 0x8b ? src : dst ^ src;
			continue;
		}
		if (op != 0x89 && op != 0x8b) break;
		static const int8_t base[8][2] = { { 3, 6 }, { 3, 7 }, { 5, 6 }, { 5, 7 }, { 6, -1 }, { 7, -1 }, { 5, -1 }, { 3, -1 } };
		uint16_t ea;
		if (mod == 0 && rm == 6) {
			ea = (uint16_t)(M[cs * 16u + ip] | M[cs * 16u + ip + 1] << 8);
			ip += 2;
		}
		else {
			ea = r[base[rm][0]] + (base[rm][1] >= 0 ? r[base[rm][1]] : 0);
			if (mod == 1) ea += (uint16_t)(int8_t)M[cs * 16u + ip++];
			if (mod == 2) {
				ea += (uint16_t)(M[cs * 16u + ip] | M[cs * 16u + ip + 1] << 8);
				ip += 2;
			}
		}
		uint32_t a = seg * 16u + ea;
		if (op == 0x89) {
			M[a] = (uint8_t)r[reg];
			M[a + 1] = (uint8_t)(r[reg] >> 8);
		}
		else r[reg] = (uint16_t)(M[a] | M[a + 1] << 8);
	}
	fprintf(stderr, "wc: the mouse event handler at %04x:%04x does more than this driver can run: not called\n",
		handler_seg, handler_off);
	return 0;
}

/* Apply the host's events (called before every INT 33h service). */
static void drain(void)
{
	uint32_t head = events_head.load(std::memory_order_relaxed);
	while (head != events_tail.load(std::memory_order_acquire)) {
		int ex = events[head % 256].x, ey = events[head % 256].y;
		int sx = ex * virt_w / 320, sy = ey * virt_h / 200;
		int nx = events[head % 256].relative ? x + sx : sx, ny = events[head % 256].relative ? y + sy : sy;
		if (!events[head % 256].relative && events[head % 256].warp != warp_wanted.load(std::memory_order_relaxed) &&
		    host_warps.load()) {
			nx = x; /* from before the host moved its pointer: only the buttons count */
			ny = y;
		}
		nx = clamp(nx, min_x, max_x);
		ny = clamp(ny, min_y, max_y);
		uint8_t nb = events[head % 256].buttons;
		head++;
		mickey_x += nx - x;
		mickey_y += (ny - y) * 2;
		x = nx;
		y = ny;
		for (int b = 0; b < 3; b++) {
			uint8_t bit = (uint8_t)(1 << b);
			if ((nb & bit) && !(buttons & bit)) {
				presses[b]++;
				press_ns[b] = pc_now_ns();
				short_click &= (uint8_t)~bit;
				press_x[b] = (int16_t)x;
				press_y[b] = (int16_t)y;
				clicked |= bit;
			}
			if (!(nb & bit) && (buttons & bit)) {
				releases[b]++;
				if (pc_now_ns() - press_ns[b] < MIN_CLICK_NS) short_click |= bit;
				release_x[b] = (int16_t)x;
				release_y[b] = (int16_t)y;
			}
		}
		buttons = nb;
	}
	events_head.store(head, std::memory_order_release);
}

/* Tell the event handler about changes (called often, from the device poll). */
void mouse_poll(void)
{
	if (!handler_mask) return;
	drain();
	uint8_t b = (uint8_t)(reported_buttons() & ~clicked) | buttons;
	uint16_t events = 0;
	if (x != handler_x || y != handler_y) events |= 1;
	if (handler_buttons >= 0) {
		for (int i = 0; i < 3; i++) {
			int was = handler_buttons >> i & 1, now = b >> i & 1;
			if (now && !was) events |= (uint16_t)(2 << (2 * i));
			if (!now && was) events |= (uint16_t)(4 << (2 * i));
		}
	}
	else events |= 1;
	handler_x = x;
	handler_y = y;
	handler_buttons = b;
	if (!(events & handler_mask)) return;
	uint16_t r[8] = { (uint16_t)(events & handler_mask), (uint16_t)x, (uint16_t)y, b, 0, 0,
		(uint16_t)mickey_x, (uint16_t)mickey_y };
	if (!real_call(handler_seg, handler_off, r)) handler_mask = 0;
}

/* The host's pointer to where the program's is now. */
static void want_warp(void)
{
	warp_x.store(x * 320 / virt_w);
	warp_y.store(y * 200 / virt_h);
	warp_wanted.fetch_add(1, std::memory_order_release);
}

void mouse_int(uint32_t at)
{
	uint16_t ax = (uint16_t)rd(R_AX, 2), bx = (uint16_t)rd(R_BX, 2), cx = (uint16_t)rd(R_CX, 2),
		dx = (uint16_t)rd(R_DX, 2);
	if (wc_trace) {
		fprintf(stderr, "wc: INT 33h AX=%04x BX=%04x CX=%04x DX=%04x at %05x\n", ax, bx, cx, dx, at);
	}
	drain();
	switch (ax) {
	case 0x00: /* reset: installed with 2 buttons (if the host has a mouse) */
	case 0x21:
		if (!mouse_seen.load() && !host_has_mouse()) {
			wr(R_AX, 2, 0);
			break;
		}
		min_x = 0; max_x = 639; min_y = 0; max_y = 199;
		x = clamp(x, min_x, max_x);
		y = clamp(y, min_y, max_y);
		visible = -1;
		handler_mask = 0;
		wr(R_AX, 2, 0xffff);
		wr(R_BX, 2, 2);
		break;
	case 0x01: if (visible < 0) visible++; break; /* show cursor */
	case 0x02: visible--; break;                  /* hide cursor */
	case 0x03: /* position and buttons */
		wr(R_BX, 2, reported_buttons());
		clicked = 0;
		wr(R_CX, 2, (uint16_t)x);
		wr(R_DX, 2, (uint16_t)y);
		break;
	case 0x04: /* set position */
		x = clamp((int16_t)cx, min_x, max_x);
		y = clamp((int16_t)dx, min_y, max_y);
		want_warp();
		break;
	case 0x05: /* button press information */
	case 0x06: { /* button release information */
		int b = bx < 3 ? bx : 0;
		wr(R_AX, 2, reported_buttons());
		if (ax == 5) {
			wr(R_BX, 2, presses[b]);
			wr(R_CX, 2, (uint16_t)press_x[b]);
			wr(R_DX, 2, (uint16_t)press_y[b]);
			presses[b] = 0;
		}
		else {
			wr(R_BX, 2, releases[b]);
			wr(R_CX, 2, (uint16_t)release_x[b]);
			wr(R_DX, 2, (uint16_t)release_y[b]);
			releases[b] = 0;
		}
		break;
	}
	case 0x07: /* horizontal range */
		min_x = (int16_t)(cx < dx ? cx : dx);
		max_x = (int16_t)(cx < dx ? dx : cx);
		if (min_x <= 0 && max_x > 0) virt_w = max_x + 1;
		x = clamp(x, min_x, max_x);
		want_warp();
		break;
	case 0x08: /* vertical range */
		min_y = (int16_t)(cx < dx ? cx : dx);
		max_y = (int16_t)(cx < dx ? dx : cx);
		if (min_y <= 0 && max_y > 0) virt_h = max_y + 1;
		y = clamp(y, min_y, max_y);
		want_warp();
		break;
	case 0x0b: /* motion counters */
		wr(R_CX, 2, (uint16_t)mickey_x);
		wr(R_DX, 2, (uint16_t)mickey_y);
		mickey_x = mickey_y = 0;
		break;
	case 0x0c: /* event handler: ES:DX for the events in CX */
		handler_seg = (uint16_t)rd(R_ES, 2);
		handler_off = dx;
		handler_mask = cx & 0x7f;
		handler_buttons = -1;
		break;
	case 0x24: /* driver version 8.0, PS/2 mouse */
		wr(R_BX, 2, 0x0800);
		wr(R_CX, 2, 0x0400);
		break;
	case 0x0a: case 0x0f: case 0x10: case 0x13: case 0x1a: case 0x1d:
		break; /* cursor shape, mickey ratio, exclusion area, speed, sensitivity, page: nothing to do */
	default:
		fprintf(stderr, "wc: unsupported mouse function INT 33h AX=%04x at %05x (ignored)\n", ax, at);
		break;
	}
}
