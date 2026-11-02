#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Assembles the GitHub Pages site in _site/: docs/ plus the engine as WebAssembly under app/
// (build it first with tools/build-web.mjs).
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const out = path.join(ROOT, '_site');
const web = path.join(ROOT, 'build', 'web');
for (const f of ['tabcore.mjs', 'tabcore.wasm', 'core.mjs']) {
  if (!fs.existsSync(path.join(web, f))) {
    console.error(`build-site: ${path.join('build', 'web', f)} is missing; run node tools/build-web.mjs first.`);
    process.exit(1);
  }
}
fs.rmSync(out, { recursive: true, force: true });
fs.cpSync(path.join(ROOT, 'docs'), out, { recursive: true });
fs.mkdirSync(path.join(out, 'app'), { recursive: true });
for (const f of ['tabcore.mjs', 'tabcore.wasm', 'core.mjs']) fs.copyFileSync(path.join(web, f), path.join(out, 'app', f));
fs.writeFileSync(path.join(out, '.nojekyll'), '');
console.log('site built in', out);
