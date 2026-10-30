/* The game port (201h) with joystick A, fed from the host's game controller. Writing the port
 * starts four one-shots whose time depends on each axis' resistance; a bit reads 1 until its
 * one-shot ends. Wing Commander III times them with the PIT (bc998: read-back of channel 0), so the
 * one-shots last emulated time, like a PC's: about 24 us to 1.1 ms, 0.57 ms centred. The CPU speed
 * limit doesn't sleep while one runs (pc.cpp), or a pause would show up as a huge reading. Joystick
 * B is absent (its bits never fall), the buttons read 0 while pressed.
 * There is a joystick only while the host has a controller (WC_JOYSTICK=1: always, =0: never):
 * with one, the game calibrates it when WC3\JOY0.DAT is missing (a text-mode screen). */
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "host.h"
#include "wc_rt.h"

uint64_t pc_now_ns(void);

enum { CENTRE_NS = 574000, RANGE_NS = 550000 };

static std::atomic<int> stick_x, stick_y, stick_roll, stick_buttons; /* -1000..1000, bit 0/1: buttons 1/2 */
static std::atomic<int> connected; /* controllers the host has */
static int forced = -2;            /* WC_JOYSTICK: 0 never, 1 always, -1 with a controller; -2 unread */
static uint64_t end_x, end_y;      /* when the one-shots end */

static int clamp(int v)
{
	return v < -1000 ? -1000 : v > 1000 ? 1000 : v;
}

void pc_joystick(int x, int y, int roll, int buttons)
{
	stick_x.store(clamp(x));
	stick_y.store(clamp(y));
	stick_roll.store(clamp(roll));
	stick_buttons.store(buttons);
}

void pc_joystick_connected(int change)
{
	connected += change;
}

static int is_present(void)
{
	if (forced == -2) {
		const char *e = getenv("WC_JOYSTICK");
		forced = e && *e ? atoi(e) != 0 : -1;
	}
	return forced >= 0 ? forced : connected.load() > 0;
}

void joystick_out(void)
{
	uint64_t now = pc_now_ns();
	end_x = now + CENTRE_NS + (int64_t)stick_x.load() * RANGE_NS / 1000;
	end_y = now + CENTRE_NS + (int64_t)stick_y.load() * RANGE_NS / 1000;
}

/* A one-shot is running: the speed limit mustn't pause now. */
int joystick_busy(void)
{
	return end_x | end_y ? pc_now_ns() < (end_x > end_y ? end_x : end_y) : 0;
}

uint8_t joystick_in(void)
{
	if (!is_present()) return 0xff; /* axes never time out, buttons up */
	uint8_t v = 0xfc; /* joystick B's axes and buttons: absent */
	uint64_t now = pc_now_ns();
	if (now < end_x) v |= 1;
	if (now < end_y) v |= 2;
	int b = stick_buttons.load();
	if (b & 1) v &= (uint8_t)~0x10;
	if (b & 2) v &= (uint8_t)~0x20;
	return v;
}
