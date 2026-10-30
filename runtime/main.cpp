/* Loads the program image, sets up the process the way the DOS extender would (flat32.cpp), and
 * enters the recompiled code at the program's entry point.
 * Usage: wc3 [game folder [game arguments...]]
 *   game arguments  passed to the game on its DOS command line
 *   WC_TRACE=1      trace DOS calls; 2 also every instruction; 3 also I/O ports
 *   WC_KEYS=tick:key,...  press keys after that many timer interrupts (else: keys typed in the terminal);
 *                   +key only presses, -key only releases
 *   WC_POKE=addr:byte,...  patch memory before starting (linear hex addresses), for experiments
 *   WC_WATCH=addr          report every write to that linear address (hex), with the callers
 *   WC_DUMP=file    save the memory at a WC_KEYS quit
 *   WC_HEADLESS=1   no window (keys from the terminal or WC_KEYS)
 *   WC_MOUSE=1      report a mouse without a window (for WC_KEYS mouse entries)
 *   WC_MOUSE_SPEED=x  the mouse's speed in full screen (default 2; Cmd+Left/Right in the window)
 *   WC_WAV=file     record the sound (AdLib, MT-32 or General MIDI, with the Sound Blaster's) to a WAV file
 *   WC_SB=0         no Sound Blaster (by default one at 220h, IRQ 5, DMA 1)
 *   WC_VESA=0       no VESA BIOS (the game then runs at 320x200 instead of 640x480)
 *   WC_GM=0         no General MIDI synth (macOS's, behind the MPU-401 when there is no WC_MT32)
 *   WC_CYCLES=n     instructions per millisecond ("max": unlimited; Cmd+Up/Down in the window)
 *   WC_FPS=n        frames per second in space flight, where frames are paced instead (default 15; 0: off;
 *                   Cmd+Up/Down in flight)
 *   WC_STATS=1      print the frame rate in flight every second
 *   WC_EMS=0        no expanded memory (EMS, 8 MB by default, as in GOG's DOSBox)
 *   WC_WARP=0       no quick start (until the first picture, or the end of a WC_KEYS script, time runs 20 times as fast)
 *   WC_CLOCK=x      instructions: emulated time from the instructions executed (default headless),
 *                   real: real time (default in the window)
 *   WC_JOYSTICK=0/1 never/always a joystick (by default while there is a game controller)
 *   WC_STICK_KEYS=b:key,...  keys a flight stick's buttons press (numbered as on the stick; default
 *                   3:enter,4:tab; "none" for none) */
#include <libgen.h>
#include <climits>
#ifndef __EMSCRIPTEN__
#include <mach-o/dyld.h>
#endif
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "host.h"
#include "wc_rt.h"

void pc_init(int tty);
extern const char *wc_game_dir;
void flat32_start(const char *dir, int argc, char **argv, void (*set_command_line)(uint8_t *, int, char **));

/* The arguments after the game folder, as the DOS command line (in the PSP, at most 126 characters). */
static void set_command_line(uint8_t *psp, int argc, char **argv)
{
	char line[127] = "";
	size_t n = 0;
	for (int i = 0; i < argc; i++) {
		int w = snprintf(line + n, sizeof line - n, " %s", argv[i]);
		if (w < 0 || n + (size_t)w >= sizeof line) {
			fprintf(stderr, "wc: the game arguments are longer than DOS allows (126 characters)\n");
			exit(1);
		}
		n += (size_t)w;
	}
	psp[0x80] = (uint8_t)n;
	memcpy(psp + 0x81, line, n);
	psp[0x81 + n] = 0x0d;
}

static void game(void)
{
	run(wc_program.flat_eip);
	fprintf(stderr, "wc: entry point returned\n");
	cpu_report(stderr);
}

static void on_signal(int sig)
{
	fprintf(stderr, "\nwc: stopped by signal %d\n", sig);
	cpu_report(stderr);
	_exit(128 + sig);
}

int main(int argc, char **argv)
{
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
#ifdef __EMSCRIPTEN__
	const char *dir = "/wc"; /* image.bin is embedded there */
#else
	char exe[PATH_MAX];
	uint32_t size = sizeof exe;
	if (_NSGetExecutablePath(exe, &size) != 0) {
		snprintf(exe, sizeof exe, "%s", argv[0]);
	}
	const char *dir = dirname(exe);
#endif

	const char *trace = getenv("WC_TRACE");
	wc_trace = trace ? atoi(trace) : 0;
	if (argc > 1) {
		wc_game_dir = argv[1];
	}
	flat32_start(dir, argc > 2 ? argc - 2 : 0, argv + 2, set_command_line);
	pc_init(!host_open());

	const char *watch = getenv("WC_WATCH");
	wc_watch = watch ? (uint32_t)strtoul(watch, NULL, 16) : 0;

	const char *poke = getenv("WC_POKE");
	for (const char *p = poke; p && *p;) {
		unsigned addr, value;
		int used = 0;
		if (sscanf(p, "%x:%x%n", &addr, &value, &used) != 2 || addr >= MEM_SIZE) {
			fprintf(stderr, "wc: bad WC_POKE at \"%s\"\n", p);
			return 1;
		}
		M[addr] = (uint8_t)value;
		p += used + (p[used] == ',');
	}

	host_run(game);
}
