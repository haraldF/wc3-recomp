# CLAUDE.md

Notes for working on this project: what was found out the hard way. The README describes the
pipeline and usage; this file records the facts and pitfalls behind it.

## Goal

A native macOS (later WebAssembly) executable of Wing Commander III (`WC3.EXE` on GOG's CD image
`../game3/DATA.DAT`), made by statically recompiling the DOS program to C. The user doesn't care
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
- Ghidra: BinaryLoader at the base, `x86:LE:32:default`, `gcc` cspec; `WcFlat32Setup.java` makes
  the entry a function before analysis. Analysis takes long (timeout 7200 s per file).

## Static recompilation (WcRecomp.java + runtime/)

- p-code → C: `R[]` is Ghidra's register space (offsets in `wc_rt.h`), `M[]` 64 MB. One C function
  per Ghidra function with an entry `switch` and gotos; code addresses are the program's offsets
  (`run()` = `run_code()`).
- Ghidra ignores segments in 32-bit code. The generator adds `wc_sb[n]` (the base of ES, CS, SS, DS,
  FS, GS's selector): SS for stack p-code (an 8-byte space constant), ES for the EDI side of string
  instructions, else the prefix's segment or DS. Segment register writes go through `seg_load()`.
- `CALLF`/`JMPF m16:32` (far through memory) are modelled as near; the generator loads the far
  pointer (23 fixes). `IRETD` like `IRET`: `set_flags_word()`.
- x87: Ghidra's 10-byte ST registers carry doubles here (x87_* helpers in `wc_rt.h`, `flat32.c`);
  PcodeOp names: FLOAT_FLOAT2FLOAT, FLOAT_INT2FLOAT, FLOAT_TRUNC. The helpers are named x87_* since
  `fcvt` collided with libc's.
- Missing code: runtime exits 4 (`wc: missing code at X`); `npm run discover` adds X to
  `entries.txt` (repository root; commit it after discover rounds) and recompiles. Bugs elsewhere
  produce garbage addresses: check an entry is real code before keeping it.
- Shadow return stack (`push_ret`/`pop_ret`/`is_return`): a jump to the address the current call
  returns to becomes a C return.
- `writeIfChanged` keeps unchanged generated files so builds stay incremental (16 KB of code per
  `seg_*.c`); each declares the functions it uses itself, only `wc_dispatch.c` includes `wc_gen.h`.
  CMake globs the generated files: the first build after a file appears or disappears fails with
  "GLOB mismatch", a second one succeeds.

## The DOS extender (runtime/flat32.c)

- Memory: the first MB as on a PC (PSP at 10000h, environment at F000h, BIOS data, video); the
  code and data selectors' offset 0 at linear 400000h, the data segment may grow to 32 MB; other
  segments (AH=48h) from 2400000h up, a bump allocator.
- Phar Lap's selectors: 0Ch code, 14h data, 1Ch screen (A0000h), 24h PSP, 2Ch environment, 34h the
  first MB. New descriptors from index 16, RPL 3. PSP +5Ch: the program's memory in bytes.
- Calls so far: INT 21h AH=48h/49h/4Ah (segments), AX=2502h-2507h (vectors), 2520h (memory
  statistics), 2526h (configuration: PSP/environment/first-MB selectors at +104h), 252Bh (page
  locking: nothing to do); DPMI 0006h-0008h. Anything else stops with exit 2 and its name.
- Other INT 21h calls go to `dos.c` with `dos_pm` set: pointers DS:EDX/DS:ESI, counts ECX, results
  EAX. Interrupts go to the program's protected-mode handler if it set one, else the BIOS code in
  `pc.c` (no real-mode code ever runs).

## State

The startup runs ~17000 instructions to code reached through a pointer (`638c0`, not yet in
`entries.txt`). Not looked at yet: the video modes (SVGA?), sound drivers, the joystick (`joystick.c`
is how WC1 reads it), the web page (`web/` is still wc-decomp's, for WC1/WC2's files).

## Conventions

- TypeScript run directly by Node (≥ 23.6), ESM, `npm run typecheck`. Java Ghidra scripts in
  `ghidra_scripts/`. C11 runtime in `runtime/`, tab-indented, short comments.
- Ghidra 12.2 (Homebrew; JAVA_HOME from the `ghidraRun` wrapper). Headless: a second
  `-scriptPath` overrides the first (join with `;`).
- Debug env vars: `WC_TRACE` (1 DOS/IRQ/extender calls, 2 every instruction, 3 more), `WC_KEYS`,
  `WC_POKE`, `WC_WATCH`, `WC_SCREENSHOT`, `WC_HEADLESS`, `WC_CYCLES`, `WC_DUMP` (see `main.c`).
  Ctrl-C prints a report.
