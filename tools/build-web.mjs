#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Builds the engine (plugin/src/core) as WebAssembly for the website and the tests:
// build/web/tabcore.mjs + tabcore.wasm + core.mjs. Needs Emscripten: em++ on PATH (as in CI), or
// EMSDK pointing at an emsdk folder (`emsdk install latest && emsdk activate latest`).
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const out = path.join(ROOT, 'build', 'web');
const core = path.join(ROOT, 'plugin', 'src', 'core');
const sources = ['containers', 'gp3to5', 'gpif', 'json', 'layout', 'model', 'synth', 'text', 'timeline'].map((n) => path.join(core, `${n}.cpp`));
sources.push(path.join(ROOT, 'plugin', 'src', 'web', 'tabcore.cpp'));

// emcc from PATH, else from EMSDK (its own Python and Node come with it)
const env = { ...process.env };
const win = process.platform === 'win32';
let emcc = 'em++';
if (env.EMSDK && fs.existsSync(env.EMSDK)) {
  const sdk = env.EMSDK;
  const sub = (dir) => fs.readdirSync(path.join(sdk, dir)).map((d) => path.join(sdk, dir, d));
  const extra = [path.join(sdk, 'upstream', 'emscripten')];
  if (fs.existsSync(path.join(sdk, 'python'))) {
    const py = sub('python')[0];
    extra.push(py);
    env.EMSDK_PYTHON = path.join(py, win ? 'python.exe' : 'bin/python3');
  }
  if (fs.existsSync(path.join(sdk, 'node'))) extra.push(path.join(sub('node')[0], 'bin'));
  env.PATH = [...extra, env.PATH].join(path.delimiter);
  env.EM_CONFIG = path.join(sdk, '.emscripten');
  const dir = path.join(sdk, 'upstream', 'emscripten');
  emcc = path.join(dir, ['em++.exe', 'em++.bat', 'em++'].find((f) => fs.existsSync(path.join(dir, f))) || 'em++');
}

fs.mkdirSync(out, { recursive: true });
const args = [
  ...sources, '-I', path.join(ROOT, 'plugin', 'src'),
  '-std=c++17', '-O2', '-fwasm-exceptions', '-Wall', '-Werror',
  '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=web,node', '-sALLOW_MEMORY_GROWTH=1', '-sFILESYSTEM=0',
  '-sEXPORTED_RUNTIME_METHODS=UTF8ToString,HEAPU8,HEAPF32',
  '-o', path.join(out, 'tabcore.mjs'),
];
try {
  execFileSync(emcc, args, { env, stdio: 'inherit', shell: win && !emcc.endsWith('.exe') });
} catch {
  console.error('build-web: Emscripten failed or was not found. Put em++ on PATH or set EMSDK to an activated emsdk folder.');
  process.exit(1);
}
fs.copyFileSync(path.join(ROOT, 'plugin', 'src', 'web', 'core.mjs'), path.join(out, 'core.mjs'));
const size = (f) => `${Math.round(fs.statSync(path.join(out, f)).size / 1024)} KB`;
console.log(`built ${path.relative(ROOT, out)}: tabcore.wasm ${size('tabcore.wasm')}, tabcore.mjs ${size('tabcore.mjs')}`);
