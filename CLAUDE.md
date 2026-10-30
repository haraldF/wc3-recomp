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
- With a joystick present (ours is while the host has a controller; `WC_JOYSTICK=1` always, `=0`
  never) and no `WC3\JOY0.DAT`, it calibrates in text mode: the prompts (centre, top left, bottom
  right, each + a button, then Esc) go to the terminal between endless "Joystick val (x, y)" lines;
  any other key skips it. It times the one-shots with the PIT (bc998, channel 0 read-back), so they
  last emulated time, and the speed limit doesn't sleep during one: before, the stick was always
  there and a sleep in the middle gave readings like 20829 instead of 48. It reads the PIT with the
  8254 read-back command (C2h: status, then count; the status' output bit tells the halves of a
  mode 3 period apart), which pc.cpp ignored: x then read short (437, 182, 54 instead of 692).
  Scripted calibration: `WC_JOYSTICK=1 WC_KEYS=1500:+j1,1600:-j1,2000:%-100.-100,2500:+j1,2600:-j1,
  3000:%100.100,3500:+j1,3600:-j1,4000:%0.0,4500:+esc,4506:-esc` (min 29, centre 685, max 1342).
- No VESA BIOS: "No VESA Driver -- Cannot use SVGA" and it takes `MCGA.DLL` (320x200) instead of
  `VESA480.DLL`; both are LX modules ("386FX", Miles Design 1992/93).
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
  game sets the range to 640x480, so the window covers the largest range set (WC1: 640x200).
- Hangar (Esc twice): hotspots show a label at the bottom when hovered ("Go to FLIGHT CONTROL." along
  the bottom edge, y 185 of 200); Alt-X asks "Exit to DOS (Y/N)?".
  `WC_MOUSE=1 WC_KEYS=3000:+esc,3006:-esc,5000:+esc,5006:-esc,9000:@160.185,9300:@160.185,
  9400:+lmb,9500:-lmb` goes to Flight Control (a click held ~100 ticks: ticks are ~860 Hz here, and
  a 6-tick click was too short for the game's frame).

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

The intro and credits play with their digital sound on the SB16; Esc twice reaches the hangar,
where the mouse works (hotspots, Flight Control), with General MIDI music on macOS. Missing: text
mode in the window, General MIDI in the browser, the web page (`web/` is still wc-decomp's).

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
