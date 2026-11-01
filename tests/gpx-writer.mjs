// SPDX-License-Identifier: Apache-2.0
// Test helper: builds Guitar Pro 6 containers (BCFS, optionally BCFZ-compressed) around a GPIF document.

const SECTOR = 0x1000;

export function buildBcfs(files) {
  // Layout: sector 0 = header area, then for each file one entry sector followed by its data sectors.
  const sectors = [new Uint8Array(SECTOR)];
  for (const [name, data] of files) {
    const entry = new Uint8Array(SECTOR);
    const dv = new DataView(entry.buffer);
    dv.setInt32(0, 2, true);
    for (let i = 0; i < name.length && i < 127; i++) entry[4 + i] = name.charCodeAt(i);
    dv.setInt32(0x8c, data.length, true);
    sectors.push(entry);
    const count = Math.ceil(data.length / SECTOR);
    for (let k = 0; k < count; k++) {
      dv.setInt32(0x94 + 4 * k, sectors.length, true);
      const s = new Uint8Array(SECTOR);
      s.set(data.subarray(k * SECTOR, (k + 1) * SECTOR));
      sectors.push(s);
    }
  }
  const out = new Uint8Array(4 + sectors.length * SECTOR);
  out.set([0x42, 0x43, 0x46, 0x53], 0); // "BCFS"
  sectors.forEach((s, i) => out.set(s, 4 + i * SECTOR));
  return out;
}

// BCFZ with literal runs plus simple back references so both decoder paths are exercised.
export function compressBcfz(bcfs) {
  const bits = [];
  const put = (v, n) => { for (let i = n - 1; i >= 0; i--) bits.push((v >> i) & 1); };
  const putRev = (v, n) => { for (let i = 0; i < n; i++) bits.push((v >> i) & 1); };
  let i = 0;
  while (i < bcfs.length) {
    // Try a back reference to a run of identical data (common in zero-padded sectors).
    let best = 0;
    let bestOff = 0;
    for (const off of [1, 4, 16, 64]) {
      if (i - off < 0) continue;
      let len = 0;
      while (len < off && len < 4095 && i + len < bcfs.length && bcfs[i + len] === bcfs[i - off + len]) len++;
      if (len > best) { best = len; bestOff = off; }
    }
    if (best >= 4) {
      const word = Math.max(Math.ceil(Math.log2(Math.max(bestOff, best) + 1)), 1);
      put(1, 1); put(word, 4); putRev(bestOff, word); putRev(best, word);
      i += best;
    } else {
      const n = Math.min(3, bcfs.length - i);
      put(0, 1); putRev(n, 2);
      for (let k = 0; k < n; k++) put(bcfs[i + k], 8);
      i += n;
    }
  }
  const body = new Uint8Array(Math.ceil(bits.length / 8));
  bits.forEach((b, k) => { if (b) body[k >> 3] |= 0x80 >> (k & 7); });
  const out = new Uint8Array(8 + body.length);
  out.set([0x42, 0x43, 0x46, 0x5a], 0); // "BCFZ"
  new DataView(out.buffer).setInt32(4, bcfs.length, true);
  out.set(body, 8);
  return out;
}
