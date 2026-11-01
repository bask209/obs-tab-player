#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Assembles the GitHub Pages site in _site/: docs/ plus the player engine under app/.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const out = path.join(ROOT, '_site');
fs.rmSync(out, { recursive: true, force: true });
fs.cpSync(path.join(ROOT, 'docs'), out, { recursive: true, filter: (src) => !src.endsWith('INSTALL.txt') });
fs.cpSync(path.join(ROOT, 'player', 'www', 'js'), path.join(out, 'app', 'js'), { recursive: true });
fs.mkdirSync(path.join(out, 'app', 'css'), { recursive: true });
fs.copyFileSync(path.join(ROOT, 'player', 'www', 'css', 'tab.css'), path.join(out, 'app', 'css', 'tab.css'));
fs.writeFileSync(path.join(out, '.nojekyll'), '');
console.log('site built in', out);
