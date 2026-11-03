# wc3-recomp

Statically recompiles Wing Commander III: Heart of the Tiger (`WC3.EXE`, a 32-bit DOS program for
the Phar Lap 386|DOS-Extender) to C++ with Ghidra, driven from TypeScript, and builds it into a
native macOS program with an emulated PC around it.

The game is fully playable on macOS, at 640x480 with its movies, digital sound and General MIDI
music. Some code addresses may still be missing: code reached only through pointers is found by
running it, so a part of the game nobody has played yet can stop with `wc: missing code at X`
(see below). Still to do: the web build (`web/` is still wc-decomp's, with no General MIDI and no
font for text mode, which the joystick calibration uses).

It grew out of wc-decomp, the same for Wing Commander I and II (16-bit real-mode programs): the
PC emulation (`runtime/`: DOS, BIOS, timer, keyboard, mouse, VGA, AdLib, MT-32, General MIDI, Sound Blaster, the
window) comes from there, and the generator is its flat 32-bit mode.

## Building it yourself

This repository contains no game files and nothing derived from them: you need your own copy of
the game, the GOG release. Everything is worked out on its `WC3.EXE` (inside the CD image
`DATA.DAT`), SHA-256 `a53c6a99917474c9be429485f745d13037bab2449680319a61e2fa054ec9b0df`; other
releases are refused.

Prerequisites (Homebrew):

```bash
brew install node ghidra cmake ninja sdl3   # Node 23.6 or later; Ghidra 12.2 brings its JDK
git submodule update --init                 # Nuked-OPL3 and munt, for AdLib and MT-32 sound
npm ci
```

Then, with GOG's game folder at `../game3` (or `-- --iso path/to/DATA.DAT` for every step):

```bash
npm run decompile    # WC3.EXE from the CD image, unpacked and analyzed with Ghidra (long)
npm run recomp       # generate C from it (out/recomp/) and build out/bin/wc3
npm run discover     # run it; code reached only through pointers is added to entries.txt
npm run play         # play it in a window
```

Addresses the game reaches that weren't recompiled stop it. `npm run play` handles that itself:
it adds the address to `entries.txt` (or captures the driver the game loaded), recompiles and
starts the game again, so you continue from your last save. Started directly, `out/bin/wc3` exits
with code 4 and the address, and `npm run discover` does the same headless.

The game writes into its folder (`WC3/`, saves, its swap file).

## How it works

- `src/p3.ts` reads `WC3.EXE` from the ISO 9660 image and unpacks its P3 load image (Phar Lap's
  run-length packing) to `out/image.bin`; Ghidra imports it as 32-bit x86 at its offset in the
  flat segment.
- `ghidra_scripts/WcRecomp.java` turns every instruction's p-code into C++ on an emulated CPU (one
  function per Ghidra function, entered at any instruction through a switch). Memory accesses add
  the base of their segment register's selector; x87 values are carried as doubles.
- `runtime/flat32.cpp` is the DOS extender: Phar Lap's selectors, its INT 21h AX=25xxh calls and
  segment memory, DPMI, protected-mode interrupts. The rest of `runtime/` is the PC.

## License

BSD 2-Clause (`LICENSE`), for this repository's code. Nuked-OPL3 (LGPL 2.1) and munt's libmt32emu
(LGPL 2.1+) in `third_party/` are linked into `out/bin/wc3`. Wing Commander III itself is not
included and remains its owners' property.
