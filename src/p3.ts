// Wing Commander III: WC3.EXE lives on the CD image (GOG's DATA.DAT, ISO 9660), and is a Phar Lap
// "P3" flat-model 32-bit program behind Origin's DPMI loader: an MZ stub, then the P3 header and a
// packed load image without relocations, linked to run at offset 1000h of a flat segment.

import { openSync, readSync, closeSync } from "node:fs";

/** A file in the root directory of an ISO 9660 image (names without the ";1" version). */
export function readIsoFile(iso: string, name: string): Buffer {
  const fd = openSync(iso, "r");
  try {
    const read = (pos: number, size: number): Buffer => {
      const b = Buffer.alloc(size);
      readSync(fd, b, 0, size, pos);
      return b;
    };
    const pvd = read(16 * 2048, 2048);
    if (pvd.toString("latin1", 1, 6) !== "CD001") throw new Error(`${iso} is not an ISO 9660 image`);
    const rootLba = pvd.readUInt32LE(156 + 2), rootSize = pvd.readUInt32LE(156 + 10);
    const dir = read(rootLba * 2048, rootSize);
    for (let p = 0; p < dir.length;) {
      const len = dir[p]!;
      if (len === 0) { // records don't cross sectors
        p = (Math.floor(p / 2048) + 1) * 2048;
        continue;
      }
      const id = dir.toString("latin1", p + 33, p + 33 + dir[p + 32]!).replace(/;\d+$/, "");
      if (id.toUpperCase() === name.toUpperCase()) {
        return read(dir.readUInt32LE(p + 2) * 2048, dir.readUInt32LE(p + 10));
      }
      p += len;
    }
    throw new Error(`no ${name} in ${iso}`);
  }
  finally {
    closeSync(fd);
  }
}

export interface P3 {
  /** the unpacked load image, to be placed at offset `base` of the flat segment */
  image: Buffer;
  base: number;
  eip: number;
  esp: number;
  /** extra memory after the image the program needs (bytes; the header counts paragraphs) */
  minExtra: number;
}

/** Parses the P3 header after the MZ stub and unpacks the load image (Phar Lap's run-length packing). */
export function parseP3(exe: Buffer): P3 {
  const mzSize = (exe.readUInt16LE(4) - 1) * 512 + exe.readUInt16LE(2);
  const h = exe.subarray(mzSize);
  if (h.toString("latin1", 0, 2) !== "P3" || h.readUInt16LE(2) !== 1) throw new Error("not a flat-model P3 program");
  const imageOff = h.readUInt32LE(0x26), imageSize = h.readUInt32LE(0x2a);
  const relocSize = h.readUInt32LE(0x18);
  if (relocSize !== 0) throw new Error("P3 relocations are not supported");
  const flags = h.readUInt16LE(0x72), memReq = h.readUInt32LE(0x74);
  const src = h.subarray(imageOff, imageOff + imageSize);
  let image: Buffer;
  if (flags & 1) { // packed: blocks of a length word; bit 15 set: a pattern (length byte, bytes) repeated
    const parts: Buffer[] = [];
    for (let p = 0; p < src.length;) {
      let n = src.readUInt16LE(p);
      p += 2;
      if (n & 0x8000) {
        n &= 0x7fff;
        const k = src[p++]!;
        const pattern = src.subarray(p, p + k);
        p += k;
        const block = Buffer.alloc(n);
        if (k) for (let i = 0; i < n; i++) block[i] = pattern[i % k]!;
        parts.push(block);
      }
      else {
        parts.push(src.subarray(p, p + n));
        p += n;
      }
    }
    image = Buffer.concat(parts);
  }
  else {
    image = Buffer.from(src);
  }
  if (image.length !== memReq) throw new Error(`unpacked ${image.length} bytes, the header says ${memReq}`);
  return {
    image,
    base: h.readUInt32LE(0x5e),
    eip: h.readUInt32LE(0x68),
    esp: h.readUInt32LE(0x62),
    minExtra: h.readUInt32LE(0x56) * 16,
  };
}
