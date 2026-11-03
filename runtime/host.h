/* The host platform around the emulated PC: a window with the VGA picture and the keyboard
 * (host_sdl.cpp; host_web.cpp in the browser). The emulation never includes SDL; it runs on a thread of its own while the host
 * owns the main thread, which macOS needs for windows (and Emscripten for the canvas). */
#pragma once
#include <cstdint>

/* The folder the program's files (image.bin) are in: the executable's. */
const char *host_base_dir(void);
/* Open the window, unless WC_HEADLESS is set or there is no display. Returns 0 when headless. */
int host_open(void);
/* Run game on a thread of its own and serve the window until the program exits. */
[[noreturn]] void host_run(void (*game)(void));

/* Provided by the emulation, called on the host's thread. */
/* The newest complete frame: its pixels (320x200 in mode 13h, 640x480 in VESA mode 101h; the size
 * into w and h) and the 6-bit palette, taken at vertical retrace. Returns a count that changes with
 * every frame. */
uint64_t vga_frame(uint8_t pixels[640 * 480], uint8_t palette[256 * 3], int *w, int *h);
/* The font text mode is drawn with (256 characters of 8x16, a byte a row), from the host. Without
 * one, text mode shows nothing. */
void vga_font(const uint8_t glyphs[256 * 16]);
/* A scancode (set 1: make code, | 0x80 for the break code) from the keyboard. */
void pc_key(uint8_t scancode);
/* CPU speed in instructions per millisecond (0: unlimited); change > 0 raises it by 20%, < 0
 * lowers it, 0 only reads it. */
uint64_t pc_cycles(int change);
/* The frame rate in space flight (0: not paced by frames), changed the same way. */
uint64_t pc_flight_fps(int change);
/* Whether frames are paced now (in flight), so pc_flight_fps sets the speed rather than pc_cycles. */
int pc_in_flight(void);
/* The mouse over the picture: x 0..319, y 0..199; buttons: 1 left, 2 right, 4 middle. */
void pc_mouse(int x, int y, int buttons);
/* Mouse movement by dx, dy picture pixels from wherever the pointer is (full screen). */
void pc_mouse_move(int dx, int dy, int buttons);
/* Where the host should move its pointer (x 0..319, y 0..199 over the picture): returns 1 when the
 * program moved the mouse's pointer or changed its range. Positions sent before the host moved its
 * pointer are ignored then (a host that never calls this keeps them all). */
int pc_mouse_warp(int *x, int *y);
/* The game controller as joystick A: x and y -1000..1000 (0 centred, -1000 left and up); roll
 * likewise (a flight stick's twist, -1000 to the left), which the PC joystick doesn't have: it sets
 * the roll rate in flight; buttons: 1 and 2 the joystick's buttons. */
void pc_joystick(int x, int y, int roll, int buttons);
/* A controller came (+1) or went (-1): the PC has a joystick while there is one. */
void pc_joystick_connected(int change);
uint8_t pc_stick_key(unsigned button); /* the key a flight stick's button (from 0) presses, 0: none */
/* Provided by the host: the program ends with this code (DOS exit, or an error), on the emulation's
 * thread. */
[[noreturn]] void host_exit(int code);
/* Provided by the host: whether there is a mouse (a window, or WC_MOUSE=1 for tests). */
int host_has_mouse(void);
/* A file in the game folder was written (on closing it) or deleted: the web host keeps such files
 * (save games) across sessions. Called on the emulation's thread. */
void host_file_saved(const char *path);
/* Sound: 16-bit stereo at rate. host_audio_open returns 0 when there is no sound output. */
int host_audio_open(unsigned rate);
void host_audio(const int16_t *frames, unsigned count);
