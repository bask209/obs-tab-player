#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Development / stand-alone server with the same HTTP API as player/tab-player.lua.
// Lets you run and test the player in any browser without OBS:
//   node tools/dev-server.mjs "C:\path\to\tabs" [port]
// No dependencies.

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const WWW = path.join(ROOT, 'player', 'www');
const LIBRARY = path.resolve(process.argv[2] || process.env.TAB_LIBRARY || path.join(ROOT, 'docs', 'demo'));
const PORT = Number(process.argv[3] || process.env.PORT || 8737);
const GP = /\.(gp|gpx|gp3|gp4|gp5|gp7|gp8)$/i;

const state = { speed: 1, loop: true, countIn: true, metronome: false, showNotes: true };
let status = {};
let library = [];

function scan() {
  const out = [];
  const walk = (dir, rel, depth) => {
    if (depth > 6) return;
    let entries = [];
    try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { return; }
    for (const e of entries) {
      if (e.name.startsWith('.') || e.name.startsWith('_')) continue;
      const r = rel ? `${rel}/${e.name}` : e.name;
      if (e.isDirectory()) walk(path.join(dir, e.name), r, depth + 1);
      else if (GP.test(e.name)) out.push(r);
    }
  };
  walk(LIBRARY, '', 0);
  const info = (id) => {
    const m = /^Day\s*(\d+)(?:\s*-\s*([A-Z]))?\s*-/i.exec(id.split('/').pop());
    return { day: m ? Number(m[1]) : null, alt: m?.[2] || '' };
  };
  library = out.map((id) => ({ id, ...info(id) })).sort((a, b) => {
    if ((a.day == null) !== (b.day == null)) return a.day == null ? 1 : -1;
    return (a.day ?? 0) - (b.day ?? 0) || a.alt.localeCompare(b.alt) || a.id.localeCompare(b.id);
  });
  if (!library.some((f) => f.id === state.fileId)) state.fileId = library[0]?.id ?? null;
}

const streams = new Set();
const broadcast = (event, data) => { for (const res of streams) res.write(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`); };
setInterval(() => { for (const res of streams) res.write(': ping\n\n'); }, 15000);

function setFile(id) { state.fileId = id; state.loopFrom = 0; state.loopTo = 0; }
function stepFile(d) {
  if (!library.length) return;
  const i = Math.max(0, library.findIndex((f) => f.id === state.fileId));
  setFile(library[(i + d + library.length) % library.length].id);
}
function stepDay(d) {
  const cur = library.find((f) => f.id === state.fileId);
  const mains = library.filter((f) => f.day != null && !f.alt);
  if (!mains.length) return stepFile(d);
  const day = cur?.day ?? 0;
  const t = d > 0 ? mains.find((f) => f.day > day) ?? mains[0] : [...mains].reverse().find((f) => f.day < day) ?? mains[mains.length - 1];
  setFile(t.id);
}

const TRANSPORT = new Set(['play', 'pause', 'playPause', 'stop', 'restart', 'seek']);
function handle({ type, values, step }) {
  if (TRANSPORT.has(type)) { broadcast('transport', { type, value: Number(step) || 0 }); return; }
  const round = (v) => Math.round(v * 100) / 100;
  switch (type) {
    case 'set': Object.assign(state, values || {}); break;
    case 'speedUp': state.speed = round(Math.min(3, (state.speed ?? 1) + (Number(step) || 0.05))); break;
    case 'speedDown': state.speed = round(Math.max(0.1, (state.speed ?? 1) - (Number(step) || 0.05))); break;
    case 'speedReset': state.speed = 1; break;
    case 'nextFile': stepFile(1); break;
    case 'prevFile': stepFile(-1); break;
    case 'nextDay': stepDay(1); break;
    case 'prevDay': stepDay(-1); break;
    case 'toggleLoop': state.loop = !state.loop; break;
    case 'toggleCountIn': state.countIn = !state.countIn; break;
    case 'toggleMetronome': state.metronome = !state.metronome; break;
    case 'toggleNotes': state.showNotes = !state.showNotes; break;
    default: return;
  }
  broadcast('state', state);
}

const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8', '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png' };
const send = (res, code, type, body) => { res.writeHead(code, { 'Content-Type': type, 'Cache-Control': 'no-cache' }); res.end(body); };
const readBody = (req) => new Promise((ok) => { let b = ''; req.on('data', (c) => { b += c; }); req.on('end', () => ok(b)); });

http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  const p = decodeURIComponent(url.pathname);
  if (!/^(127\.0\.0\.1|localhost):\d+$/.test(req.headers.host || '')) return send(res, 403, 'text/plain', 'Forbidden host');
  if (p === '/') { res.writeHead(302, { Location: '/dock.html' }); return res.end(); }
  if (p === '/events') {
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-store', Connection: 'keep-alive' });
    res.write(`event: state\ndata: ${JSON.stringify(state)}\n\nevent: status\ndata: ${JSON.stringify(status)}\n\n`);
    streams.add(res);
    req.on('close', () => streams.delete(res));
    return;
  }
  if (p === '/api/library') {
    if (url.searchParams.has('rescan')) { scan(); broadcast('library', {}); }
    return send(res, 200, 'application/json', JSON.stringify({ root: LIBRARY, files: library.map((f) => ({ id: f.id })) }));
  }
  if (p === '/api/state') return send(res, 200, 'application/json', JSON.stringify(state));
  if (p === '/api/file') {
    const id = url.searchParams.get('id') || '';
    if (!library.some((f) => f.id === id)) return send(res, 404, 'text/plain', 'Unknown file');
    return fs.readFile(path.join(LIBRARY, id), (err, data) => (err ? send(res, 404, 'text/plain', 'Cannot read') : send(res, 200, 'application/octet-stream', data)));
  }
  if (p === '/api/status' && req.method === 'POST') { status = JSON.parse((await readBody(req)) || '{}'); broadcast('status', status); return send(res, 204, 'text/plain', ''); }
  if (p === '/api/command' && req.method === 'POST') {
    try { handle(JSON.parse(await readBody(req))); } catch { return send(res, 400, 'text/plain', 'Bad JSON'); }
    return send(res, 200, 'application/json', '{"ok":true}');
  }
  const cmd = /^\/api\/command\/(\w+)$/.exec(p);
  if (cmd) { handle({ type: cmd[1], step: url.searchParams.get('step') }); return send(res, 200, 'application/json', '{"ok":true}'); }
  const file = path.resolve(WWW, '.' + p);
  if (!file.startsWith(WWW + path.sep)) return send(res, 403, 'text/plain', 'Forbidden');
  fs.readFile(file, (err, data) => (err ? send(res, 404, 'text/plain', 'Not found') : send(res, 200, MIME[path.extname(file)] || 'application/octet-stream', data)));
}).listen(PORT, '127.0.0.1', () => {
  scan();
  console.log(`Tab Player dev server: ${library.length} tabs in ${LIBRARY}`);
  console.log(`  Overlay: http://127.0.0.1:${PORT}/overlay.html`);
  console.log(`  Dock:    http://127.0.0.1:${PORT}/dock.html`);
});
