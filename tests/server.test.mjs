// SPDX-License-Identifier: Apache-2.0
// HTTP API tests against tools/dev-server.mjs (the same API the OBS Lua script serves).
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import path from 'node:path';
import http from 'node:http';

const ROOT = path.join(import.meta.dirname, '..');
const PORT = 18737;
const base = `http://127.0.0.1:${PORT}`;
let server;

before(async () => {
  server = spawn(process.execPath, [path.join(ROOT, 'tools', 'dev-server.mjs'), path.join(ROOT, 'docs', 'demo'), String(PORT)], { stdio: 'ignore' });
  for (let i = 0; i < 50; i++) {
    try { if ((await fetch(base + '/api/state')).ok) return; } catch { /* starting */ }
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error('dev server did not start');
});
after(() => server?.kill());

const cmd = (body) => fetch(base + '/api/command', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });

test('lists the library and serves files', async () => {
  const lib = await (await fetch(base + '/api/library')).json();
  assert.equal(lib.files.length, 5);
  const res = await fetch(base + '/api/file?id=' + encodeURIComponent(lib.files[0].id));
  assert.equal(res.status, 200);
  assert.ok((await res.arrayBuffer()).byteLength > 100);
  assert.equal((await fetch(base + '/api/file?id=..%2F..%2Fpackage.json')).status, 404);
});

test('serves the pages and blocks path traversal and foreign hosts', async () => {
  for (const p of ['/overlay.html', '/dock.html', '/js/overlay.js', '/js/core/render.js', '/css/tab.css']) assert.equal((await fetch(base + p)).status, 200, p);
  assert.notEqual((await fetch(base + '/..%2F..%2FLICENSE')).status, 200);
  // DNS-rebinding guard: requests that claim another host are refused (fetch cannot set Host)
  const status = await new Promise((ok) => http.get({ host: '127.0.0.1', port: PORT, path: '/api/state', headers: { Host: 'evil.example' } }, (r) => { r.resume(); ok(r.statusCode); }));
  assert.equal(status, 403);
});

test('commands change state and stream to listeners', async () => {
  const events = [];
  const ac = new AbortController();
  const stream = fetch(base + '/events?role=test', { signal: ac.signal }).then(async (r) => {
    const reader = r.body.getReader();
    let buf = '';
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      buf += new TextDecoder().decode(value);
      let i;
      while ((i = buf.indexOf('\n\n')) >= 0) {
        const block = buf.slice(0, i);
        buf = buf.slice(i + 2);
        const ev = /event: (\w+)\ndata: (.*)/.exec(block);
        if (ev) events.push({ event: ev[1], data: JSON.parse(ev[2]) });
      }
    }
  }).catch(() => {});
  await new Promise((r) => setTimeout(r, 100));
  await cmd({ type: 'set', values: { speed: 1, fileId: 'Demo 01 - Alternate Picking.gp5' } });
  await cmd({ type: 'speedUp' });
  await cmd({ type: 'speedUp', step: 0.1 });
  await cmd({ type: 'nextFile' });
  await cmd({ type: 'toggleMetronome' });
  await fetch(base + '/api/command/restart');
  await cmd({ type: 'seek', step: 2.5 });
  await new Promise((r) => setTimeout(r, 150));
  ac.abort();
  await stream;
  const state = await (await fetch(base + '/api/state')).json();
  assert.equal(state.speed, 1.15);
  assert.equal(state.fileId, 'Demo 02 - Legato.gp5');
  assert.equal(state.metronome, true);
  assert.ok(events.some((e) => e.event === 'state'));
  assert.ok(events.some((e) => e.event === 'transport' && e.data.type === 'restart'));
  assert.ok(events.some((e) => e.event === 'transport' && e.data.type === 'seek' && e.data.value === 2.5));
  assert.equal((await cmd({ type: 'set', values: { speed: 1 } })).status, 200);
  assert.equal((await fetch(base + '/api/command', { method: 'POST', body: '{nope' })).status, 400);
});
