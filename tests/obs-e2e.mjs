#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// End-to-end test inside a real, portable OBS Studio (Windows):
//  - starts OBS with a scene collection that loads tab-player.lua and the overlay,
//  - selects a tab, starts recording, plays it, stops recording,
//  - reports OBS performance stats, CPU/memory of OBS + browser processes, and log errors.
//
// Usage: node tests/obs-e2e.mjs --obs <portable OBS folder> --library <tab folder>
//        [--file "Day 004 - Arpeggios.gp5"] [--seconds 15] [--port 8740] [--theme light]
// The OBS folder must be a copy of an OBS install with config in <obs>/config (portable mode).

import { spawn, execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const args = Object.fromEntries(process.argv.slice(2).reduce((a, v, i, all) => (v.startsWith('--') ? [...a, [v.slice(2), all[i + 1]]] : a), []));
const OBS = path.resolve(args.obs || '');
const LIB = path.resolve(args.library || '');
const FILE = args.file || 'Day 004 - Arpeggios.gp5';
const SECONDS = Number(args.seconds || 15);
const PORT = Number(args.port || 8740);
const WS_PORT = 4466;
const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const exe = path.join(OBS, 'bin', '64bit', 'obs64.exe');
if (!fs.existsSync(exe)) { console.error('obs64.exe not found under --obs'); process.exit(2); }

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const api = (p, body) => fetch(`http://127.0.0.1:${PORT}${p}`, body ? { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) } : undefined);

// --- write the scene collection pointing at this repo
const cfgDir = path.join(OBS, 'config', 'obs-studio');
const scenes = path.join(cfgDir, 'basic', 'scenes', 'Test.json');
const coll = JSON.parse(fs.readFileSync(scenes, 'utf8'));
coll.modules['scripts-tool'] = [{ path: path.join(REPO, 'player', 'tab-player.lua').replace(/\\/g, '/'), settings: { library: LIB.replace(/\\/g, '/'), port: PORT } }];
coll.sources.find((s) => s.id === 'browser_source').settings.url = `http://127.0.0.1:${PORT}/${args.page || 'overlay.html'}`;
if (args.noscript) {
  coll.modules['scripts-tool'] = [];
  Object.assign(coll.sources.find((s) => s.id === 'browser_source').settings, { is_local_file: true, local_file: path.join(REPO, 'player', 'www', args.page || 'overlay.html').split(path.sep).join('/') });
} else {
  delete coll.sources.find((s) => s.id === 'browser_source').settings.is_local_file;
}
fs.writeFileSync(scenes, JSON.stringify(coll, null, 1));
const logDir = path.join(cfgDir, 'logs');
const logsBefore = new Set(fs.existsSync(logDir) ? fs.readdirSync(logDir) : []);

// --- obs-websocket v5 client (Node has a global WebSocket)
async function obsSocket() {
  for (let i = 0; i < 60; i++) {
    try {
      const ws = new WebSocket(`ws://127.0.0.1:${WS_PORT}`);
      const pending = new Map();
      let n = 0;
      await new Promise((ok, fail) => {
        ws.onerror = fail;
        ws.onmessage = (e) => {
          const m = JSON.parse(e.data);
          if (m.op === 0) ws.send(JSON.stringify({ op: 1, d: { rpcVersion: 1, eventSubscriptions: 0 } }));
          else if (m.op === 2) ok();
          else if (m.op === 7) { pending.get(m.d.requestId)?.(m.d); pending.delete(m.d.requestId); }
        };
      });
      return {
        call: (requestType, requestData) => new Promise((ok) => {
          const requestId = String(++n);
          pending.set(requestId, ok);
          ws.send(JSON.stringify({ op: 6, d: { requestType, requestId, requestData } }));
        }),
        close: () => ws.close(),
      };
    } catch { await sleep(500); }
  }
  throw new Error('obs-websocket did not come up');
}

function processStats() {
  const ps = `Get-Process | Where-Object { $_.Path -like '${OBS.replace(/'/g, "''")}*' } | Select-Object Name,Id,CPU,WorkingSet64 | ConvertTo-Json -Compress`;
  try {
    const out = execFileSync('powershell.exe', ['-NoProfile', '-Command', ps], { encoding: 'utf8' }).trim();
    const list = out ? [].concat(JSON.parse(out)) : [];
    return list.reduce((a, p) => {
      const key = p.Name;
      a[key] = a[key] || { count: 0, cpu: 0, mem: 0 };
      a[key].count++; a[key].cpu += p.CPU || 0; a[key].mem += p.WorkingSet64 || 0;
      return a;
    }, {});
  } catch { return {}; }
}
const diff = (a, b, secs) => Object.fromEntries(Object.keys(b).map((k) => [k, {
  processes: b[k].count,
  cpuPercentOfOneCore: Math.round((((b[k].cpu - (a[k]?.cpu || 0)) / secs) * 1000)) / 10,
  memoryMB: Math.round(b[k].mem / 1048576),
}]));

// Read the overlay's latest status from the event stream.
async function readStatus() {
  const ac = new AbortController();
  try {
    const r = await fetch(`http://127.0.0.1:${PORT}/events?role=probe`, { signal: ac.signal });
    const rd = r.body.getReader();
    let t = '';
    for (;;) {
      const { value, done } = await rd.read();
      if (done) return null;
      t += new TextDecoder().decode(value);
      const m = /event: status\ndata: (.*)\n/.exec(t);
      if (m) { ac.abort(); return JSON.parse(m[1]); }
    }
  } catch { return null; }
}

// Finds the playback cursor in a BMP screenshot: the column with the longest vertical run of
// cursor-blue pixels (chord names share the colour but are short).
function analyseBmp(buf) {
  if (buf.toString('latin1', 0, 2) !== 'BM') return { cursorX: null };
  const off = buf.readUInt32LE(10);
  const w = buf.readInt32LE(18);
  const hRaw = buf.readInt32LE(22);
  const h = Math.abs(hRaw);
  const bpp = buf.readUInt16LE(28) / 8;
  const stride = Math.ceil((w * bpp) / 4) * 4;
  let best = { x: null, run: 0 };
  let changed = 0;
  for (let x = 0; x < w; x++) {
    let run = 0;
    let max = 0;
    for (let y = 0; y < h; y++) {
      const row = hRaw > 0 ? h - 1 - y : y;
      const i = off + row * stride + x * bpp;
      const b = buf[i];
      const g = buf[i + 1];
      const r = buf[i + 2];
      if (b > 170 && r < 120 && g > 60 && g < 160) { run++; if (run > max) max = run; } else run = 0;
    }
    if (max > best.run) best = { x, run: max };
    if (max > 0) changed++;
  }
  return { cursorX: best.run >= 30 ? best.x : null, cursorHeight: best.run, width: w };
}

// --- run
console.log('Starting OBS (portable)…');
const obs = spawn(exe, ['--portable', ...(args.visible ? [] : ['--minimize-to-tray']), '--disable-updater', '--disable-shutdown-check', '--multi'], { cwd: path.dirname(exe), stdio: 'ignore' });
let result = { ok: false };
try {
  if (args.noscript) {
    const ws = await obsSocket();
    await sleep(3000);
    await ws.call('StartRecord');
    const item = (await ws.call('GetSceneItemId', { sceneName: 'Scene', sourceName: 'Tab Player' })).responseData?.sceneItemId;
    for (let k = 0; k < SECONDS * 4; k++) { await ws.call('SetSceneItemTransform', { sceneName: 'Scene', sceneItemId: item, sceneItemTransform: { positionX: (k * 40) % 600, positionY: 0 } }); await sleep(250); }
    const stop = await ws.call('StopRecord');
    const st = (await ws.call('GetStats')).responseData;
    ws.close();
    result = { ok: true, recording: stop.responseData?.outputPath, stats: st };
    throw Object.assign(new Error('done'), { done: true });
  }
  let up = false;
  for (let i = 0; i < 120 && !up; i++) { try { up = (await api('/api/state')).ok; } catch { await sleep(500); } }
  if (!up) throw new Error('Tab Player server (Lua script) did not start');
  const lib = await (await api('/api/library')).json();
  console.log(`Lua server up, ${lib.files.length} tabs listed`);
  const ws = await obsSocket();
  await api('/api/command', { type: 'set', values: { fileId: FILE, speed: 1, loop: true, countIn: true, metronome: false, theme: args.theme || 'light', loopFrom: 0, loopTo: 0 } });
  await sleep(2500); // let the overlay load the file and the synth render its buffers
  const status0 = await (await api('/api/state')).json();
  const idleA = processStats();
  await sleep(3000);
  const idleB = processStats();
  const startRec = await ws.call('StartRecord');
  if (!startRec.requestStatus?.result) throw new Error('StartRecord failed: ' + JSON.stringify(startRec.requestStatus));
  await sleep(500);
  if (args.probe) { await sleep(1000); await api('/api/command', { type: 'set', values: { theme: 'dark' } }); await sleep(1500); }
  const tStart = Date.now();
  await api('/api/command/restart');
  const playA = processStats();
  const statsA = (await ws.call('GetStats')).responseData;
  // Screenshots of the browser source rendered by OBS, paired with the overlay's own status.
  const shots = [];
  const until = Date.now() + SECONDS * 1000;
  while (Date.now() < until) {
    const shot = await ws.call('GetSourceScreenshot', { sourceName: 'Tab Player', imageFormat: 'bmp' });
    const st = await readStatus();
    const img = shot.responseData?.imageData;
    if (img) shots.push({ at: (Date.now() - tStart) / 1000, time: st?.time, bar: st?.bar, reportedX: st?.diag?.cursorX, ...analyseBmp(Buffer.from(img.split(',')[1], 'base64')) });
    await sleep(400);
  }
  const playB = processStats();
  const lastStatus = await readStatus();
  const statsB = (await ws.call('GetStats')).responseData;
  const stop = await ws.call('StopRecord');
  const playedFor = (Date.now() - tStart) / 1000;
  await sleep(1500);
  ws.close();
  result = {
    ok: true,
    file: status0.fileId,
    recording: stop.responseData?.outputPath,
    playedSeconds: playedFor,
    overlayStatus: lastStatus,
    screenshots: shots,
    idle: diff(idleA, idleB, 3),
    playing: diff(playA, playB, SECONDS),
    obsStats: {
      cpuUsage: statsB.cpuUsage,
      memoryMB: Math.round(statsB.memoryUsage),
      averageFrameRenderMs: statsB.averageFrameRenderTime,
      renderSkippedFrames: statsB.renderSkippedFrames - statsA.renderSkippedFrames,
      outputSkippedFrames: statsB.outputSkippedFrames - statsA.outputSkippedFrames,
      activeFps: statsB.activeFps,
    },
  };
} catch (e) {
  if (!e.done) result.error = e.message;
} finally {
  obs.kill();
  await sleep(1500);
}
// OBS log: script messages and errors
const newLogs = fs.existsSync(logDir) ? fs.readdirSync(logDir).filter((f) => !logsBefore.has(f)) : [];
const log = newLogs.map((f) => fs.readFileSync(path.join(logDir, f), 'utf8')).join('\n');
result.logLines = log.split(/\r?\n/).filter((l) => /tab.player|\[Lua\]|script/i.test(l) && !/^\s*$/.test(l)).slice(0, 30);
result.logErrors = log.split(/\r?\n/).filter((l) => /error|failed/i.test(l) && /lua|tab|browser|script/i.test(l)).slice(0, 20);
console.log(JSON.stringify(result, null, 2));
process.exit(result.ok ? 0 : 1);
