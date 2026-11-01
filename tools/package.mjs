#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Builds dist/obs-tab-player-<version>.zip: the folder users extract and load in OBS.
// Uses a stored (uncompressed) zip so no dependencies are needed.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const { version } = JSON.parse(fs.readFileSync(path.join(ROOT, 'package.json'), 'utf8'));
const name = `obs-tab-player-${version}`;
const files = [];
const add = (abs, rel) => files.push([rel, fs.readFileSync(abs)]);
const walk = (dir, rel) => {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    if (e.name.startsWith('_') || e.name.startsWith('.')) continue;
    const a = path.join(dir, e.name);
    const r = `${rel}/${e.name}`;
    if (e.isDirectory()) walk(a, r); else add(a, r);
  }
};
walk(path.join(ROOT, 'player'), name);
add(path.join(ROOT, 'LICENSE'), `${name}/LICENSE`);
add(path.join(ROOT, 'docs', 'INSTALL.txt'), `${name}/INSTALL.txt`);
for (const f of fs.readdirSync(path.join(ROOT, 'docs', 'demo'))) if (f.endsWith('.gp5')) add(path.join(ROOT, 'docs', 'demo', f), `${name}/demo-tabs/${f}`);

const CRC = new Int32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c; });
const crc32 = (b) => { let c = -1; for (const x of b) c = CRC[(c ^ x) & 0xff] ^ (c >>> 8); return (c ^ -1) >>> 0; };
const parts = []; const central = []; let offset = 0;
for (const [rel, data] of files) {
  const nameB = Buffer.from(rel);
  const crc = crc32(data);
  const h = Buffer.alloc(30);
  h.writeUInt32LE(0x04034b50, 0); h.writeUInt16LE(20, 4); h.writeUInt16LE(0x0800, 6);
  h.writeUInt32LE(crc, 14); h.writeUInt32LE(data.length, 18); h.writeUInt32LE(data.length, 22); h.writeUInt16LE(nameB.length, 26);
  const c = Buffer.alloc(46);
  c.writeUInt32LE(0x02014b50, 0); c.writeUInt16LE(20, 4); c.writeUInt16LE(20, 6); c.writeUInt16LE(0x0800, 8);
  c.writeUInt32LE(crc, 16); c.writeUInt32LE(data.length, 20); c.writeUInt32LE(data.length, 24); c.writeUInt16LE(nameB.length, 28); c.writeUInt32LE(offset, 42);
  parts.push(h, nameB, data);
  central.push(c, nameB);
  offset += 30 + nameB.length + data.length;
}
const cd = Buffer.concat(central);
const end = Buffer.alloc(22);
end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(files.length, 8); end.writeUInt16LE(files.length, 10); end.writeUInt32LE(cd.length, 12); end.writeUInt32LE(offset, 16);
fs.mkdirSync(path.join(ROOT, 'dist'), { recursive: true });
const out = path.join(ROOT, 'dist', `${name}.zip`);
fs.writeFileSync(out, Buffer.concat([...parts, cd, end]));
console.log(`${out} (${files.length} files, ${(fs.statSync(out).size / 1024).toFixed(0)} KB)`);
