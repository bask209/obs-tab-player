// SPDX-License-Identifier: Apache-2.0
// Guitar Pro 6 (.gpx) container: "BCFZ" compressed or "BCFS" plain sector file system.

export function isGpx(bytes) {
  const h = String.fromCharCode(bytes[0], bytes[1], bytes[2], bytes[3]);
  return h === 'BCFZ' || h === 'BCFS';
}

const int32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24));

function decompressBcfz(src) {
  const expected = int32(src, 4);
  const out = new Uint8Array(Math.max(0, expected));
  let len = 0;
  let pos = 8 * 8; // bit position, after "BCFZ" + length
  const total = src.length * 8;
  const bit = () => {
    if (pos >= total) throw RangeError('eof');
    const b = (src[pos >> 3] >> (7 - (pos & 7))) & 1;
    pos++;
    return b;
  };
  const bits = (n) => { let v = 0; for (let i = 0; i < n; i++) v = (v << 1) | bit(); return v; };
  const bitsReversed = (n) => { let v = 0; for (let i = 0; i < n; i++) v |= bit() << i; return v; };
  try {
    while (len < expected) {
      if (bit()) {
        const word = bits(4);
        const offset = bitsReversed(word);
        const size = bitsReversed(word);
        const from = len - offset;
        const n = Math.min(offset, size);
        for (let i = 0; i < n && len < expected; i++) out[len++] = out[from + i];
      } else {
        const size = bitsReversed(2);
        for (let i = 0; i < size && len < expected; i++) out[len++] = bits(8);
      }
    }
  } catch (e) {
    if (!(e instanceof RangeError)) throw e;
  }
  return out.subarray(0, len);
}

// Returns a Map of file name -> bytes.
export function readGpxFiles(bytes) {
  const header = String.fromCharCode(bytes[0], bytes[1], bytes[2], bytes[3]);
  let data;
  if (header === 'BCFZ') {
    const full = decompressBcfz(bytes);
    data = full.subarray(4); // skip inner "BCFS"
  } else {
    data = bytes.subarray(4);
  }
  const files = new Map();
  const SECTOR = 0x1000;
  let offset = SECTOR;
  while (offset + 3 < data.length) {
    if (int32(data, offset) === 2) {
      let name = '';
      for (let i = 0; i < 127; i++) {
        const c = data[offset + 4 + i];
        if (!c) break;
        name += String.fromCharCode(c);
      }
      const size = int32(data, offset + 0x8c);
      const chunks = [];
      const pointers = offset + 0x94;
      for (let k = 0; ; k++) {
        const sector = int32(data, pointers + 4 * k);
        if (!sector) break;
        offset = sector * SECTOR;
        chunks.push(data.subarray(offset, offset + SECTOR));
      }
      const fileData = new Uint8Array(Math.max(0, size));
      let p = 0;
      for (const c of chunks) {
        const n = Math.min(c.length, size - p);
        if (n <= 0) break;
        fileData.set(c.subarray(0, n), p);
        p += n;
      }
      files.set(name, fileData);
    }
    offset += SECTOR;
  }
  return files;
}
