#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Writes the demo exercises to docs/demo/ as Guitar Pro 5 files.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { writeGp } from './gp-writer.mjs';
import { DEMO_SONGS } from './demo-songs.mjs';

const out = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', 'docs', 'demo');
fs.mkdirSync(out, { recursive: true });
for (const s of DEMO_SONGS) {
  const file = path.join(out, `${s.title}.gp5`);
  fs.writeFileSync(file, writeGp(s, '5.10'));
  console.log('wrote', path.relative(process.cwd(), file));
}
fs.writeFileSync(path.join(out, 'index.json'), JSON.stringify(DEMO_SONGS.map((s) => `${s.title}.gp5`), null, 1) + '\n');
