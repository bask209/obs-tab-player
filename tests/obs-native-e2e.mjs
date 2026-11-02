#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// End-to-end test of the native plugin in a real, portable OBS Studio (Windows).
// Installs build/plugin/Release/obs-tab-player.dll into the portable OBS, opens a scene with a
// Tab Player source, plays a tab via the plugin's hotkeys (through obs-websocket), measures CPU,
// memory and frame stats, records, grabs frames, and reports log errors.
//
// node tests/obs-native-e2e.mjs --obs <portable OBS> --library <tabs> [--file "Day 004 - Arpeggios.gp5"]
//      [--seconds 15] [--theme dark] [--frames <dir>] [--horizontal 0] [--metronome 1] [--width 1920 --height 400]
import { spawn, execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const args = Object.fromEntries(process.argv.slice(2).reduce((a, v, i, all) => (v.startsWith('--') ? [...a, [v.slice(2), all[i + 1]]] : a), []));
const OBS = path.resolve(args.obs || '');
const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const SECONDS = Number(args.seconds || 15);
const exe = path.join(OBS, 'bin', '64bit', 'obs64.exe');
if (!fs.existsSync(exe)) { console.error('obs64.exe not found under --obs'); process.exit(2); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// --- install the plugin and its settings into the portable OBS
const dll = path.join(REPO, 'build', 'plugin', 'Release', 'obs-tab-player.dll');
fs.copyFileSync(dll, path.join(OBS, 'obs-plugins', '64bit', 'obs-tab-player.dll'));
fs.cpSync(path.join(REPO, 'plugin', 'data'), path.join(OBS, 'data', 'obs-plugins', 'obs-tab-player'), { recursive: true });
const cfg = path.join(OBS, 'config', 'obs-studio');
const pcfg = path.join(cfg, 'plugin_config', 'obs-tab-player');
fs.mkdirSync(pcfg, { recursive: true });
fs.writeFileSync(path.join(pcfg, 'settings.json'), JSON.stringify({
  library: path.resolve(args.library || '').replace(/\\/g, '/'), fileId: args.file || 'Day 004 - Arpeggios.gp5', theme: args.theme || 'dark',
  speed: 1, loop: true, countIn: true, metronome: args.metronome === '1', horizontal: args.horizontal !== '0', colorStrings: true,
  ...(args.floating === '1' ? { background: 0, showPanel: false } : {}),
}));
const sceneFile = path.join(cfg, 'basic', 'scenes', 'Test.json');
const coll = JSON.parse(fs.readFileSync(sceneFile, 'utf8'));
coll.modules['scripts-tool'] = [];
coll.sources = [
  { id: 'scene', versioned_id: 'scene', name: 'Scene', enabled: true, flags: 0, mixers: 0, volume: 1, settings: { id_counter: 1, items: [{ name: 'Tab Player', id: 1, visible: true, locked: false, pos: { x: 0, y: 0 }, scale: { x: 1, y: 1 }, rot: 0, align: 5, bounds_type: 0, bounds: { x: 0, y: 0 } }] } },
  { id: 'tab_player_source', versioned_id: 'tab_player_source', name: 'Tab Player', enabled: true, flags: 0, mixers: 255, volume: 1, settings: { width: Number(args.width || 1920), height: Number(args.height || 400) } },
  { id: 'tab_player_metronome', versioned_id: 'tab_player_metronome', name: 'Tab Player Metronome', enabled: true, flags: 0, mixers: 255, volume: 1, settings: {} },
];
coll.sources[0].settings.items.push({ name: 'Tab Player Metronome', id: 3, visible: true, locked: false, pos: { x: 0, y: 0 }, scale: { x: 1, y: 1 }, rot: 0, align: 5, bounds_type: 0, bounds: { x: 0, y: 0 } });
coll.sources[0].settings.id_counter = 3;
if (args.backdrop) { // the tab as a strip across the bottom of a 1280x720 canvas
  const w = Number(args.width || 1920), h = Number(args.height || 400), k = 1280 / w;
  Object.assign(coll.sources[0].settings.items[0], { pos: { x: 0, y: 720 - h * k }, scale: { x: k, y: k } });
}
// --backdrop <image>: an image behind the tab (e.g. a camera still), and frames show the whole scene
if (args.backdrop) {
  coll.sources.push({ id: 'image_source', versioned_id: 'image_source', name: 'Backdrop', enabled: true, flags: 0, mixers: 0, volume: 1, settings: { file: path.resolve(args.backdrop).split(path.sep).join('/') } });
  coll.sources[0].settings.items.unshift({ name: 'Backdrop', id: 2, visible: true, locked: false, pos: { x: 0, y: 0 }, scale: { x: 1, y: 1 }, rot: 0, align: 5, bounds_type: 0, bounds: { x: 0, y: 0 } });
  coll.sources[0].settings.id_counter = 2;
}
fs.writeFileSync(sceneFile, JSON.stringify(coll, null, 1));
const logDir = path.join(cfg, 'logs');
const before = new Set(fs.existsSync(logDir) ? fs.readdirSync(logDir) : []);

async function obsSocket() {
  for (let i = 0; i < 80; i++) {
    try {
      const ws = new WebSocket('ws://127.0.0.1:4466');
      const pending = new Map();
      let n = 0;
      const meters = {};
      let metering = false;
      await new Promise((ok, fail) => {
        ws.onerror = fail;
        ws.onmessage = (e) => {
          const m = JSON.parse(e.data);
          if (m.op === 0) ws.send(JSON.stringify({ op: 1, d: { rpcVersion: 1, eventSubscriptions: 1 << 16 } })); // InputVolumeMeters
          else if (m.op === 5 && m.d.eventType === 'InputVolumeMeters' && metering)
            for (const inp of m.d.eventData.inputs) {
              const peak = Math.max(0, ...inp.inputLevelsMul.map((c) => c[1]));
              meters[inp.inputName] = Math.max(meters[inp.inputName] || 0, peak);
            }
          else if (m.op === 2) ok();
          else if (m.op === 7) { pending.get(m.d.requestId)?.(m.d); pending.delete(m.d.requestId); }
        };
      });
      return {
        call: (requestType, requestData) => new Promise((ok) => { const id = String(++n); pending.set(id, ok); ws.send(JSON.stringify({ op: 6, d: { requestType, requestId: id, requestData } })); }),
        close: () => ws.close(),
        meter: (on) => { metering = on; return meters; },
      };
    } catch { await sleep(500); }
  }
  throw new Error('obs-websocket did not come up');
}

function processStats() {
  const ps = `Get-Process | Where-Object { $_.Path -like '${OBS.replace(/'/g, "''")}*' } | Select-Object Name,CPU,WorkingSet64,PrivateMemorySize64,HandleCount | ConvertTo-Json -Compress`;
  try {
    const out = execFileSync('powershell.exe', ['-NoProfile', '-Command', ps], { encoding: 'utf8' }).trim();
    return (out ? [].concat(JSON.parse(out)) : []).reduce((a, p) => {
      a[p.Name] = a[p.Name] || { count: 0, cpu: 0, mem: 0, priv: 0, handles: 0 };
      a[p.Name].count++; a[p.Name].cpu += p.CPU || 0; a[p.Name].mem += p.WorkingSet64 || 0; a[p.Name].priv += p.PrivateMemorySize64 || 0; a[p.Name].handles += p.HandleCount || 0;
      return a;
    }, {});
  } catch { return {}; }
}
const diff = (a, b, s) => Object.fromEntries(Object.keys(b).map((k) => [k, { processes: b[k].count, cpuPercentOfOneCore: Math.round(((b[k].cpu - (a[k]?.cpu || 0)) / s) * 1000) / 10, memoryMB: Math.round(b[k].mem / 1048576), privateMB: Math.round(b[k].priv / 1048576), handles: b[k].handles }]));

console.log('Starting OBS (portable) with the native plugin…');
const obs = spawn(exe, ['--portable', '--minimize-to-tray', '--disable-updater', '--disable-shutdown-check', '--multi'], { cwd: path.dirname(exe), stdio: 'ignore' });
const result = { ok: false };
try {
  const ws = await obsSocket();
  await sleep(4000);
  const idleA = processStats(); await sleep(5000); const idleB = processStats();
  if (!args.norecord) {
    const rec = await ws.call('StartRecord');
    if (!rec.requestStatus?.result) throw new Error('StartRecord failed');
  }
  const recWall = Date.now();
  await sleep(300);
  const hk = await ws.call('TriggerHotkeyByName', { hotkeyName: 'tab_player.play_pause' });
  if (!hk.requestStatus?.result) throw new Error('hotkey failed: ' + JSON.stringify(hk.requestStatus));
  const playWall = Date.now();
  ws.meter(true);
  const statsA = (await ws.call('GetStats')).responseData;
  const playA = processStats();
  await sleep(SECONDS * 1000);
  const playB = processStats();
  const statsB = (await ws.call('GetStats')).responseData;
  const peaks = ws.meter(false);
  result.peakDb = Object.fromEntries(Object.entries(peaks).map(([k, v]) => [k, v > 0 ? Math.round(20 * Math.log10(v) * 10) / 10 : -Infinity]));
  // frames (after the measurement, so the screenshots do not disturb it)
  const frames = [];
  if (args.frames) {
    fs.mkdirSync(args.frames, { recursive: true });
    await ws.call('TriggerHotkeyByName', { hotkeyName: 'tab_player.restart' });
    const restartWall = Date.now();
    const until = Date.now() + Number(args.frameSeconds || 12) * 1000;
    while (Date.now() < until) {
      const t0 = Date.now();
      const shot = await ws.call('GetSourceScreenshot', { sourceName: args.backdrop ? 'Scene' : 'Tab Player', imageFormat: 'jpg', imageCompressionQuality: 90 });
      const img = shot.responseData?.imageData;
      if (img) {
        const name = `f${String(frames.length).padStart(4, '0')}.jpg`;
        fs.writeFileSync(path.join(args.frames, name), Buffer.from(img.split(',')[1], 'base64'));
        frames.push({ name, t: ((t0 + Date.now()) / 2 - recWall) / 1000, sinceRestart: ((t0 + Date.now()) / 2 - restartWall) / 1000 });
      }
    }
    fs.writeFileSync(path.join(args.frames, 'frames.json'), JSON.stringify(frames));
  }
  await ws.call('TriggerHotkeyByName', { hotkeyName: 'tab_player.play_pause' });
  const stop = args.norecord ? {} : await ws.call('StopRecord');
  await sleep(1000);
  const afterStop = processStats();
  ws.close();
  Object.assign(result, {
    ok: true,
    recording: stop.responseData?.outputPath,
    playStartedAfterRecordStart: (playWall - recWall) / 1000,
    idle: diff(idleA, idleB, 5),
    playing: diff(playA, playB, SECONDS),
    memoryAfter: diff(afterStop, afterStop, 1),
    obs: { cpuUsage: statsB.cpuUsage, averageFrameRenderMs: statsB.averageFrameRenderTime, renderSkippedFrames: statsB.renderSkippedFrames - statsA.renderSkippedFrames, outputSkippedFrames: statsB.outputSkippedFrames - statsA.outputSkippedFrames, activeFps: statsB.activeFps },
    frames: frames.length,
  });
} catch (e) {
  result.error = e.message;
} finally {
  obs.kill();
  await sleep(1500);
}
const log = (fs.existsSync(logDir) ? fs.readdirSync(logDir).filter((f) => !before.has(f)) : []).map((f) => fs.readFileSync(path.join(logDir, f), 'utf8')).join('\n');
result.pluginLog = log.split(/\r?\n/).filter((l) => /tab-player|obs-tab-player/i.test(l)).slice(0, 20);
result.errors = log.split(/\r?\n/).filter((l) => /error|failed|crash|exception/i.test(l) && /tab/i.test(l)).slice(0, 20);
console.log(JSON.stringify(result, null, 2));
process.exit(result.ok ? 0 : 1);
