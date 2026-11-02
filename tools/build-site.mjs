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

// llms-full.txt: llms.txt plus the text of every page, for AI assistants that read one file.
const pageText = (file) => {
  let h = fs.readFileSync(path.join(ROOT, 'docs', file), 'utf8');
  h = h.slice(h.indexOf('<main'), h.indexOf('</main>'));
  h = h.replace(/<section class="board"[\s\S]*?<\/section>/, '') // the in-browser demo's controls
    .replace(/<nav[\s\S]*?<\/nav>/g, '')
    .replace(/<(video|figure)[\s\S]*?<\/\1>/g, '')
    .replace(/<h1[^>]*>/g, '\n# ').replace(/<h2[^>]*>/g, '\n## ').replace(/<h3[^>]*>/g, '\n### ')
    .replace(/<span class="num">(\d+)<\/span>/g, '$1. ')
    .replace(/<li[^>]*>/g, '\n- ').replace(/<dt[^>]*>/g, '\n\n**').replace(/<\/dt>/g, '**\n')
    .replace(/<\/(p|h\d|dd|div)>/g, '\n').replace(/<br>/g, '\n')
    .replace(/<a [^>]*href="([^"]+)"[^>]*>([\s\S]*?)<\/a>/g, (m, href, t) => `[${t}](${new URL(href, 'https://bask209.github.io/obs-tab-player/')})`)
    .replace(/<\/?(b|code)>/g, (m) => (m.includes('code') ? '`' : '**'))
    .replace(/<[^>]+>/g, '')
    .replace(/&amp;/g, '&').replace(/&lt;/g, '<').replace(/&gt;/g, '>')
    .replace(/[ \t]+/g, ' ').replace(/\n /g, '\n').replace(/\n{3,}/g, '\n\n');
  return h.trim();
};
const pages = [['index.html', 'https://bask209.github.io/obs-tab-player/'], ['start.html', 'https://bask209.github.io/obs-tab-player/start.html']];
fs.writeFileSync(path.join(out, 'llms-full.txt'), [fs.readFileSync(path.join(ROOT, 'docs', 'llms.txt'), 'utf8').trim(),
  ...pages.map(([f, url]) => `---\n\nSource: ${url}\n\n${pageText(f)}`)].join('\n\n') + '\n');
console.log('site built in', out);
