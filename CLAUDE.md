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
- With a joystick present (ours always is unless `WC_JOYSTICK=0`) it shows a text-mode joystick
  test grid ("Joystick val (x, y)") and waits.
- No VESA BIOS: "No VESA Driver -- Cannot use SVGA" and it takes `MCGA.DLL` (320x200) instead of
  `VESA480.DLL`; both are LX modules ("386FX", Miles Design 1992/93).
- Sound: `hmidrv.386` on the CD is a library of HMI SOS drivers, one per card; the game loads the
  one `WC3.CFG` names into memory and far-calls it (selector AFh, offset 0).

## Static recompilation (WcRecomp.java + runtime/)

- p-code → C: `R[]` is Ghidra's register space (offsets in `wc_rt.h`), `M[]` 64 MB. One C function
  per Ghidra function with an entry `switch` and gotos; code addresses are the program's offsets
  (`run()` = `run_code()`).
- Ghidra ignores segments in 32-bit code. The generator adds `wc_sb[n]` (the base of ES, CS, SS, DS,
  FS, GS's selector): SS for stack p-code (an 8-byte space constant), ES for the EDI side of string
  instructions, else the prefix's segment or DS. Segment register writes go through `seg_load()`.
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
- Calls so far: INT 21h AH=48h/49h/4Ah (segments; 48h with 0 pages for a segment that 250Ah then
  maps), AX=2502h-2507h (vectors), 250Ah (map physical memory: the offset in EAX), 2511h (issue a
  real-mode interrupt: segment registers, EAX, EDX in a block), 2517h (the DOS data buffer, here
  9000:0000), 2520h (memory statistics), 2526h (configuration: PSP/environment/first-MB selectors
  at +104h), 252Bh (page locking: nothing to do), 2531h (read/write an LDT descriptor), 2532h/2533h
  (exception handlers), 25C0h-25C2h (conventional memory, 2000:0-9000:0); DPMI 0006h-0008h.
  Anything else stops with exit 2 and its name.
- Physical memory is linear memory here: a 250Ah mapping of an empty segment moves its base there.
- Other INT 21h calls go to `dos.c` with `dos_pm` set: pointers DS:EDX/DS:ESI, counts ECX, results
  EAX. Interrupts go to the program's protected-mode handler if it set one, else the BIOS code in
  `pc.c` (no real-mode code ever runs).

## The CD (runtime/iso.c, dos.c)

- `DATA.DAT` is drive D: (`WC_CD=file` for another image): ISO 9660, primary volume descriptor
  names only. Files there open read-only through a `dup` of the image's descriptor, read with
  `pread`. Per-drive current directories; the program starts on D:. MSCDEX: 1500h, 150Bh, 150Ch,
  150Dh (also through 2511h from protected mode, with real-mode ES:BX).

## State

With `WC_JOYSTICK=0`, ~2.1 M instructions: text-mode loading messages, the CD found, all `.TRE`
archives opened, config read, video and sound drivers loaded; then the first far call into
loaded code (the HMI driver from `hmidrv.386`) stops it ("missing code at 00000", CS=00AF). Code
loaded at runtime needs what wc-decomp does for its drivers (capture or pre-recompile, dispatch
by selector base). Nothing drawn yet. Not looked at yet: the joystick (`joystick.c` is how WC1
reads it), the web page (`web/` is still wc-decomp's).

## Conventions

- TypeScript run directly by Node (≥ 23.6), ESM, `npm run typecheck`. Java Ghidra scripts in
  `ghidra_scripts/`. C11 runtime in `runtime/`, tab-indented, short comments.
- Ghidra 12.2 (Homebrew; JAVA_HOME from the `ghidraRun` wrapper). Headless: a second
  `-scriptPath` overrides the first (join with `;`).
- Debug env vars: `WC_TRACE` (1 DOS/IRQ/extender calls, 2 every instruction, 3 more), `WC_KEYS`,
  `WC_POKE`, `WC_WATCH`, `WC_SCREENSHOT`, `WC_HEADLESS`, `WC_CYCLES`, `WC_DUMP` (see `main.c`).
  Ctrl-C prints a report.
