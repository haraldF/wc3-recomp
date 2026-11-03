# CLAUDE.md

Notes for working on this project: what was found out the hard way. The README describes the
pipeline and usage; this file records the facts and pitfalls behind it.

## Goal

A native macOS (later WebAssembly) executable of Wing Commander III (`WC3.EXE` on GOG's CD image
`../game3/DATA.DAT`), made by statically recompiling the DOS program to C++. The user doesn't care
about naming or understanding the code, only about running it.

Split off from wc-decomp (`../wc-decomp`, Wing Commander I/II, 16-bit real mode), whose runtime
this one started from; its CLAUDE.md has the background on the PC emulation (timer pacing, VGA
retrace, sound devices, the window, the web build).

Never commit game files or anything derived from their bytes: `DATA.DAT`, `WC3.EXE`, the unpacked
image (`image.bin`), Ghidra projects, screenshots, ROM images. Everything under `out/` is
generated and ignored.

## The executable

- `WC3.EXE` is in the root directory of the ISO 9660 image `DATA.DAT` (GOG mounts it in DOSBox);
  the game folder also has `WC3/WC3.CFG`. GOG's DOSBox: `cycles=max`, `memsize=30`, S3 SVGA.
- MetaWare High C, Phar Lap 386|DOS-Extender, flat model: an MZ stub (Origin's DPMI loader), then
  the "P3" header (format word 1 = flat). Offsets in that header: image offset 26h and size 2Ah,
  relocation size 18h (zero here), flags 72h (bit 0: packed), unpacked size 74h, minimum extra
  memory 56h (in paragraphs), base 5Eh (1000h: the image's offset in the segment), ESP 62h
  (129160h), EIP 68h (9D2DCh). Packing: blocks led by a length word; bit 15 set means a pattern
  (length byte, bytes) repeated to that length.
- Header +5Ah: maximum extra memory, FFFFFFFFh (all there is). Given only the minimum, the game's
  heap was ~1.8 MB and it stopped with "Not enough high memory": it wants 4.3 MB of free heap
  (9b164), and the C library grows the segment by ~4.4 MB beyond the program's memory (PSP +5Ch)
  right at startup, so that can't be all of it either: it gets 16 MB of a 32 MB segment.
- Ghidra: BinaryLoader at the base, `x86:LE:32:default`, `gcc` cspec; `WcFlat32Setup.java` makes
  the entry a function before analysis. Analysis takes long (timeout 7200 s per file).
- GOG starts the game on D: (the CD) with C: holding `WC3\WC3.CFG`; the game switches to C:, tests
  that it can write there (`C:\TESTE.TST`), puts `SWAPFILE.$$$` in C:'s root, copies `WC3.CON`
  from the CD to `C:\WC3`, looks for each `.TRE` archive on C: first, then on the CD drive MSCDEX
  reports (150Dh: drive numbers, A: = 0).
- It writes into the game folder: test with a scratch copy (`WC3/` copied, `DATA.DAT` and
  `OBJECTS.TRE` linked), passed as `--iso copy/DATA.DAT`.
- The stick steers only with "joystick" ticked under control in the flight options (Alt-O; `save`
  keeps it): with "mouse" (the user's config) the game ignores it. Scripted: Alt-O, click (48,63),
  exit (183,152), then `%x.y` (checked: the ship turns, the pilot's hand moves).
- With a joystick present (ours is while the host has a controller; `WC_JOYSTICK=1` always, `=0`
  never) and no `WC3\JOY0.DAT`, it calibrates in text mode: the prompts (centre, top left, bottom
  right, each + a button, then Esc) under a grid, the reading printed again and again ("Joystick
  val (x, y)", also in the terminal); any other key skips it. Alt-J (hangar, flight) shows the same
  screen (a240, called from five places), ending with the scaled reading, (0, 0) when centred, until
  Esc: in the window it was black, which looked like a hang. It times the one-shots with the PIT (bc998, channel 0 read-back), so they
  last emulated time, and the speed limit doesn't sleep during one: before, the stick was always
  there and a sleep in the middle gave readings like 20829 instead of 48. It reads the PIT with the
  8254 read-back command (C2h: status, then count; the status' output bit tells the halves of a
  mode 3 period apart), which pc.cpp ignored: x then read short (437, 182, 54 instead of 692).
  Scripted calibration: `WC_JOYSTICK=1 WC_KEYS=1500:+j1,1600:-j1,2000:%-100.-100,2500:+j1,2600:-j1,
  3000:%100.100,3500:+j1,3600:-j1,4000:%0.0,4500:+esc,4506:-esc` (min 29, centre 685, max 1342).
- Graphics: `VESA480.DLL` (640x480) with a VESA BIOS, else `MCGA.DLL` (320x200, "No VESA Driver --
  Cannot use SVGA"); both are LX modules ("386FX", Miles Design 1992/93). `vga.cpp` is a VBE 1.2
  BIOS with mode 101h only (`WC_VESA=0`: none): the game asks 4F01h through 2511h (ES:DI in real
  mode: `dos_real`), sets it with 4F02h, uses 4F07h (display start), and the DLL switches banks
  through the mode's window function (F000:0100, `bios_far_call`) with Phar Lap 2510h (a real-mode
  call with the registers in a block). 512 KB of video memory; a bank switch copies the 64 KB window
  at A0000h out and the new bank in. Frames carry their size (`vga_frame`); `WC_SCREENSHOT` files
  hold 640x480 pixels then. The mouse stays in 320x200 terms, scaled to the game's 640x480 range,
  so the hotspot positions below hold in both modes.
- Text mode (modes 0-3, `vga.cpp`): 80x25 from B8000h drawn at 640x400 with the EGA colours (DAC
  0-15 as the BIOS sets them; no attribute controller mapping, blink shown steady), the cursor from
  the BIOS data area (40:50h, shape 40:60h; the game hides it with AH=01h). The game writes the grid
  into B8000h and prints through DOS (handle 1, the C library adds CR): DOS console output goes
  through a BIOS teletype there too, with AH=02h/03h positioning. No VGA BIOS font: `font_ttf.cpp`
  draws code page 437 from a TrueType font with SDL_ttf (`fonts/*.ttf`, copied to `out/bin/font.ttf`;
  `WC_FONT` overrides): the largest size whose line fits 16 pixels and 'M' 8, threshold 100 on the
  smoothed glyphs, no fallback for characters the font lacks; the line and block characters
  B0h-DFh by hand so they join. Without a font only those show. The web build has none yet.
  Text doesn't end the quick start (only a visible graphics picture does). With a window, DOS's
  console output goes to the text screen only (`dos_echo`): the calibration rewrites its reading in
  place, which flooded the terminal; the text screen is printed to the terminal at the exit instead.
  Headless, it still goes to the terminal too.
- Movies at 640x480 are 240 lines, shown doubled: the game sets bit 7 of CRTC register 9 (3D4h/3D5h,
  maximum scan line) and toggles the sequencer's screen-off bit (3C4h index 1, bit 5). Ignored,
  they filled the top 200 lines. `vga_picture` repeats lines by register 9.
- Flight: the space backdrop is drawn by a texture mapper (b3757..) that patches its row table's
  address into its own displacements (`MOV ES:[0xb375a],EAX`, 15 of them); recompiled with the
  placeholder (-1) it drew black. The generator's patched-constant handling (drivers before) now
  covers the program too, including Ghidra's `0 + c` for ES:[c].
- Sound: `hmidrv.386` on the CD is a library of HMI SOS drivers, one per card; the game loads the
  ones it needs into conventional memory blocks (25C0h), makes data and code selectors for them
  (2531h) and far-calls them. GOG's `WC3.CFG` (dwords): music device A001h (MPU-401, General MIDI)
  at 330h, digital E016h at 220h, IRQ 5, DMA 1 and 5 (a Sound Blaster 16).
- The MPU-401 detection driver has a bug on its timeout path (a `PUSH ECX` popped twice, then a
  stray `POP DS`): harmless where an MPU-401 answers, so one always does (`mt32.cpp`, `WC_MPU=0`
  off); its MIDI goes nowhere without `WC_MT32` (no General MIDI synth yet).
- The video DLL (MCGA.DLL) finds the CRT controller by reading the Miscellaneous Output register
  (3CCh, bit 0); without it, it waited for a retrace on 3BAh forever.
- The digital driver needs a Sound Blaster 16 (41h rate, Bxh/Cxh transfers, DMA channel 5): the
  intro's movie waits for its sound blocks. It never sends D1h (speaker on): an SB16's output is
  always on, and the runtime muted it, so blocks played silently (check with `WC_WAV`, not by DSP
  activity: the intro was taken for audible since its blocks ran). The mixer (d6, 16002431) takes
  its source with `LDS ESI,[ESI+8]`, which the generator read through SS (see below): it mixed the
  interrupt table into the DMA buffer (32000h).
- Music: General MIDI to the MPU-401 (the intro's sound is all digital; the hangar has music).
  Without `WC_MT32` the MIDI goes to macOS's own General MIDI synth (`gm.cpp`: AudioToolbox's DLS
  synth, rendered on the emulation thread with AudioUnitRender, no output unit, 44.1 kHz float
  non-interleaved; `WC_GM=0` off), which then has the output the Sound Blaster mixes into. Hangar
  music peaks ~3000 alone (`WC_SB=0`), ~7500 with the SB's sound. No General MIDI in the web build
  yet (`gm.cpp` is empty there).
- Mouse: the game installs an event handler (INT 33h AX=0Ch through 2511h, mask 7Fh), real-mode
  code it copies to a 2-paragraph block: it stores BX, CX, DX at its offsets 0, 2, 4, and the game
  polls them (ca448), never AX=3. `mouse.cpp` runs such handlers with a small 16-bit interpreter
  (push/pop, mov/xor, prefixes, RETF; it reports anything else) whenever the state changes. The
  game sets the range to 640x480, but 0..1278 in flight (two units a pixel): the window covers the
  last range set from 0 (one starting further in only confines the pointer). Keeping the largest
  made the options screen (Alt-O, 0..640) put the pointer at twice the x. A real driver keeps its
  numbers when the range changes: flight starts where "Fly MISSION." was clicked (about x 600 of
  0..639), near the middle of 0..1278. Mapping the window afresh put it at the right edge (~1200)
  and the ship turned from the start. So on a range change or AX=4 the host moves the Mac's
  (hidden) pointer to the program's (`pc_mouse_warp`, SDL_WarpMouseInWindow; not in full screen,
  which is relative anyway), and positions sent before that are ignored.
- Hangar (Esc twice): hotspots show a label at the bottom when hovered ("Go to FLIGHT CONTROL." along
  the bottom edge, y 185 of 200); Alt-X asks "Exit to DOS (Y/N)?".
  `WC_MOUSE=1 WC_KEYS=3000:+esc,3006:-esc,5000:+esc,5006:-esc,9000:@160.185,9300:@160.185,
  9400:+lmb,9500:-lmb` goes to Flight Control (a click held ~100 ticks: ticks are ~860 Hz here, and
  a 6-tick click was too short for the game's frame).
- Loading a save (`00000000.WSG` + `WSG_NDX.WSG` in the folder's root; the callsign is in its PLYR
  chunk): Flight Control's Main Terminal (180,40), the callsign + Enter, Duty Logs (60,155), the slot
  (first at 75,52), Load (50,176): "GAME LOADED.", Return (255,176), Logoff (160,176). Scripted after
  the Flight Control click above: `14000:@180.40,14400:+lmb,14500:-lmb`, letters from 17000 every 80
  ticks (held 30), then 18900, 20900, 21900, 23900, 25400 for the clicks (checked on both clocks).
- After loading, "Fly MISSION." is at the right edge of Flight Control (300,100); Esc skips the
  launch scene into the cockpit, in the launch bay. Take off with A (the manual's auto-takeoff):
  full throttle there flies into the bay. Script (after the load steps, with the mouse centred):
  `27500:@300.100,27900:+lmb,28000:-lmb,30000:+esc,30010:-esc,38000:@160.100,40000:+a,40100:-a,
  50000:+a,50100:-a` (autopilot), then Space (guns), t (target), Enter (missile).
- Afterburner: with a mouse, Tab does nothing; that's the game. Each frame its keyboard routine sets
  the afterburner bit (4cccd, from Tab's key state at 124D33h) and then its mouse routine rebuilds
  the bit from its own state (4da79). With the mouse it is the right button tapped, then held
  (`+rmb`, `-rmb` 30 ticks later, `+rmb` again 50 after that): 1200 kps.

## Static recompilation (WcRecomp.java + runtime/)

- p-code → C++: `R[]` is Ghidra's register space (offsets in `wc_rt.h`), `M[]` 64 MB. One C function
  per Ghidra function with an entry `switch` and gotos; code addresses are the program's offsets
  (`run()` = `run_code()`).
- Ghidra ignores segments in 32-bit code. The generator adds `wc_sb[n]` (the base of ES, CS, SS, DS,
  FS, GS's selector): SS for stack p-code (an 8-byte space constant), ES for the EDI side of string
  instructions, else the prefix's segment or DS. LDS/LES/LFS/LGS/LSS load their far pointer through
  an 8-byte space constant too but are ordinary operands (21 of them were read through SS; harmless
  in the program, where SS = DS, not in drivers with their own DS). Segment register writes go through `seg_load()`.
- `CALLF`/`JMPF m16:32` (far through memory) are modelled as near; the generator loads the far
  pointer (23 fixes). `IRETD` like `IRET`: `set_flags_word()`.
- `LSL r32` keeps only the limit's low word in Ghidra's model (`Reg32 = zext(tmp:2)`): the C
  library sizes its heap from DS's limit, got 0BFFFh and filled 4 GB with FFh. The generator emits
  LSL itself (`seg_limit32()`).
- Memory operands based on EBP or ESP default to SS, not DS: the C library's far copy (9cf8c) does
  `LDS ESI,...` and then reads its count from `[EBP+18h]`. Harmless while DS = SS, which is why it
  showed only there.
- The C library's `int86()` writes `INT n; RET` on the stack and calls it: `run()` recognises those
  bytes and does the interrupt and the return.
- x87: Ghidra's 10-byte ST registers carry doubles here (x87_* helpers in `wc_rt.h`, `flat32.cpp`);
  PcodeOp names: FLOAT_FLOAT2FLOAT, FLOAT_INT2FLOAT, FLOAT_TRUNC. The helpers are named x87_* since
  `fcvt` collided with libc's.
- Recompiling: each captured driver costs a Ghidra run, so `doRecomp` redoes a driver only when its
  bytes, its entries or `WcRecomp.java` changed (`drv_<name>.stamp` in `out/recomp/`).
- Missing code: runtime exits 4 (`wc: missing code at X`); `npm run discover` adds X to
  `entries.txt` (repository root; commit it after discover rounds) and recompiles. Bugs elsewhere
  produce garbage addresses: check an entry is real code before keeping it.
- Shadow return stack (`push_ret`/`pop_ret`/`is_return`): a jump to the address the current call
  returns to becomes a C return. A RET/IRET to an address a frame further up expects unwinds to it
  (`ret_to`, `wc_unwind`; each call site does `if (pop_ret()) return;`): the timer handler's last
  "callback" (a31c7) ends the interrupt with IRETD instead of returning.

## Code loaded at runtime (drivers)

- Exit 5: the runtime saves the code (`WC_LOADED_DIR`) and `discover` names it by a hash of its
  first 4 KB (`drivers.txt`, kept in the repository: dN, plus the offset for flat ones), imports it
  into Ghidra (`WC/loaded`, `WcDriverSetup.java` seeds its entries, no function start search) and
  recompiles it (`WcRecomp.java driver`: `drv_dN.cpp`, `drv_dN_tables.cpp`; `wc_drivers.cpp` lists
  them).
- Two kinds: with a code selector of their own (the HMI drivers in conventional memory), recompiled
  at 10000000h + n * 1000000h and found by their bytes at that selector's base; and "flat" ones
  loaded into the program's own memory and called through its code selector (the 386FX DLLs),
  recompiled at the offset they were captured from (a 128 KB window), so their relative calls into
  the program stay right.
- Recognised by their instruction bytes. The same memory holds different code over time (the game
  loaded one HMI driver, then another over it): of the drivers whose bytes are there, the one with
  an instruction at the target wins; if none has one and memory differs from every capture there,
  it is new code. Capturing stale bytes once made text decode as code (`140023ff`, removed).
- Inside recompiled drivers, CALL pushes the virtual address: return addresses at 10000000h and up
  are code addresses as they are (`code_addr`, `run`). Code that reads its own address (`call $+5;
  pop`) would see the virtual one.
- Self-modifying drivers: `MOV FS:[1972h],FS` writes the data selector into a `MOV AX,0FFFFh`.
  Stores to fixed offsets inside the driver's own instructions are found (`findPatches`, also
  FS/GS-based: Ghidra adds its FS_OFFSET register); such constants are read from memory at runtime
  and their bytes left out of the recognition.
- Selector 0 once made `discover` capture "a driver" from address 0 (d3 then; removed): check new
  captures make sense.
- `writeIfChanged` keeps unchanged generated files so builds stay incremental (16 KB of code per
  `seg_*.cpp`); each declares the functions it uses itself, only `wc_dispatch.cpp` includes `wc_gen.h`.
  CMake globs the generated files: the first build after a file appears or disappears fails with
  "GLOB mismatch", a second one succeeds.

## The DOS extender (runtime/flat32.cpp)

- Memory: the first MB as on a PC (PSP at 10000h, environment at F000h, BIOS data, video); the
  code and data selectors' offset 0 at linear 400000h, the data segment may grow to 32 MB; other
  segments (AH=48h) from 2400000h up, a bump allocator.
- Phar Lap's selectors: 0Ch code, 14h data, 1Ch screen (A0000h), 24h PSP, 2Ch environment, 34h the
  first MB. New descriptors from index 16, RPL 3. PSP +5Ch: the program's memory in bytes.
- Calls so far: INT 21h AH=48h/49h/4Ah (segments; 48h with 0 pages for a segment that 250Ah then
  maps), AX=2502h-2507h (vectors), 250Ah (map physical memory: the offset in EAX), 2511h (issue a
  real-mode interrupt: segment registers, EAX, EDX in a block), 2517h (the DOS data buffer, here
  9000:0000), 2520h (memory statistics), 2526h (configuration: PSP/environment/first-MB selectors
  at +104h), 252Bh (page locking: nothing to do), 2531h (read/write an LDT descriptor), 2532h/2533h
  (exception handlers), 25C0h-25C2h (conventional memory, 2000:0-9000:0); DPMI 0006h-0008h.
  Anything else stops with exit 2 and its name.
- Physical memory is linear memory here: a 250Ah mapping of an empty segment moves its base there.
- Other INT 21h calls go to `dos.cpp` with `dos_pm` set: pointers DS:EDX/DS:ESI, counts ECX, results
  EAX. Interrupts go to the program's protected-mode handler if it set one, else the BIOS code in
  `pc.cpp` (no real-mode code ever runs).

## The CD (runtime/iso.cpp, dos.cpp)

- `DATA.DAT` is drive D: (`WC_CD=file` for another image): ISO 9660, primary volume descriptor
  names only. Files there open read-only through a `dup` of the image's descriptor, read with
  `pread`. Per-drive current directories; the program starts on D:. MSCDEX: 1500h, 150Bh, 150Ch,
  150Dh (also through 2511h from protected mode, with real-mode ES:BX).

## State

At 640x480 (VESA): the intro and credits with their digital sound on the SB16, the hangar and
Flight Control with the mouse, General MIDI music on macOS, loading a save at a Main Terminal,
take-off, flight with the backdrop, autopilot, guns, targeting and missiles (scripted, no crash).
Text mode (joystick calibration) in the window. Missing: text mode's font and General MIDI in the browser, the web page (`web/` is still
wc-decomp's; `web_frame_width/height` give the frame's size).

## Conventions

- TypeScript run directly by Node (≥ 23.6), ESM, `npm run typecheck`. Java Ghidra scripts in
  `ghidra_scripts/`. C++23 runtime in `runtime/` and generated code (Apple clang 21, Emscripten),
  tab-indented, short comments; still mostly C-style from wc-decomp, made idiomatic as it is
  touched. munt through its C interface (`MT32EMU_API_TYPE 1`). File-scope `const` has internal
  linkage in C++: the generated tables are declared `extern` in `wc_rt.h`. A full build of the
  generated code takes ~70 s (no slower than as C).
- Ghidra 12.2 (Homebrew; JAVA_HOME from the `ghidraRun` wrapper). Headless: a second
  `-scriptPath` overrides the first (join with `;`).
- Debug env vars: `WC_TRACE` (1 DOS/IRQ/extender calls, 2 every instruction, 3 more), `WC_KEYS`,
  `WC_POKE`, `WC_WATCH`, `WC_SCREENSHOT`, `WC_HEADLESS`, `WC_CYCLES`, `WC_DUMP` (see `main.cpp`).
  Ctrl-C prints a report.
