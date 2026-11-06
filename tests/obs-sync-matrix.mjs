#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Audio/video sync of the plugin in a real OBS (Windows), across video and audio settings.
// For each setting a portable OBS gets a fresh profile, records a few seconds of a tab playing,
// and the recording is measured: where the cursor is (from the video frames) at the moment each
// note starts (from the audio), against where that note is drawn. 0 ms = the cursor is exactly on
// the note when you hear it.
//
//   node tests/obs-sync-matrix.mjs --obs <portable OBS folder> [--seconds 15] [--only 1080p60]
//        [--loopGap 1] [--keep] [--json results.json]
//
// The portable OBS needs the plugin installed (system-wide, or in its obs-plugins folder) and
// ffmpeg/ffprobe on PATH. Its config folder is rewritten; recordings are deleted after measuring
// unless --keep is given.
import { spawn, spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { writeGp } from '../tools/gp-writer.mjs';
import { STANDARD } from '../tools/demo-songs.mjs';

const args = Object.fromEntries(process.argv.slice(2).reduce((a, v, i, all) => (v.startsWith('--') ? [...a, [v.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : '1']] : a), []));
const OBS = path.resolve(args.obs || '');
const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const SECONDS = Number(args.seconds || 15);
const exe = path.join(OBS, 'bin', '64bit', 'obs64.exe');
if (!fs.existsSync(exe)) { console.error('obs64.exe not found under --obs'); process.exit(2); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// name, canvas, output (scaled), fps, sample rate, speakers
const MATRIX = [
  ['1080p30 48k stereo', 1920, 1080, 1920, 1080, '30', 48000, 'Stereo'],
  ['1080p60', 1920, 1080, 1920, 1080, '60', 48000, 'Stereo'],
  ['1080p24', 1920, 1080, 1920, 1080, '24 NTSC', 48000, 'Stereo'],
  ['1080p29.97', 1920, 1080, 1920, 1080, '29.97', 48000, 'Stereo'],
  ['720p30', 1280, 720, 1280, 720, '30', 48000, 'Stereo'],
  ['1440p30', 2560, 1440, 2560, 1440, '30', 48000, 'Stereo'],
  ['4K30', 3840, 2160, 3840, 2160, '30', 48000, 'Stereo'],
  ['1080p canvas, 720p output', 1920, 1080, 1280, 720, '30', 48000, 'Stereo'],
  ['vertical 1080x1920', 1080, 1920, 1080, 1920, '30', 48000, 'Stereo'],
  ['44.1 kHz', 1920, 1080, 1920, 1080, '30', 44100, 'Stereo'],
  ['mono', 1920, 1080, 1920, 1080, '30', 48000, 'Mono'],
  ['5.1 surround', 1920, 1080, 1920, 1080, '30', 48000, '5.1'],
].filter((m) => !args.only || m[0].includes(args.only));
const FPS = { 30: 30, 60: 60, '24 NTSC': 24000 / 1001, 29.97: 30000 / 1001 };

const cfg = path.join(OBS, 'config', 'obs-studio');
const recDir = path.join(OBS, 'recordings');
// the tab under test: two bars of evenly spaced eighths on one string, 100 BPM, so every note
// has its own column and its own clear start in the audio
const library = path.join(OBS, 'tabs');
fs.mkdirSync(library, { recursive: true });
const eighths = (frets) => ({ voices: [frets.map((fret) => ({ value: 8, notes: [{ string: 1, fret }] }))] });
fs.writeFileSync(path.join(library, 'Sync test.gp5'), writeGp({
  title: 'Sync test', artist: '', tempo: 100, masterBars: [{ num: 4, den: 4 }, { num: 4, den: 4 }],
  tracks: [{ name: 'Guitar', tuning: STANDARD, bars: [eighths([0, 3, 0, 3, 0, 3, 0, 3]), eighths([5, 3, 5, 3, 5, 3, 5, 3])] }],
}, '5.10'));
const ini = (sections) => '﻿' + Object.entries(sections).map(([s, kv]) => `[${s}]\n` + Object.entries(kv).map(([k, v]) => `${k}=${v}`).join('\n')).join('\n\n') + '\n';
const write = (f, s) => { fs.mkdirSync(path.dirname(f), { recursive: true }); fs.writeFileSync(f, s); };

function configure([, cw, ch, ow, oh, fps, rate, speakers]) {
  fs.rmSync(cfg, { recursive: true, force: true });
  fs.mkdirSync(recDir, { recursive: true });
  write(path.join(cfg, 'global.ini'), ini({ General: { LastVersion: 553648129, 'Pre33.0Migrated': 'true', EnableAutoUpdates: 'false' } }));
  write(path.join(cfg, 'user.ini'), ini({ General: { FirstRun: 'false', ConfirmOnExit: 'false', Pre19Defaults: 'false', Pre21Defaults: 'false', Pre23Defaults: 'false', 'Pre24.1Defaults': 'false' }, Basic: { Profile: 'Test', ProfileDir: 'Test', SceneCollection: 'Test', SceneCollectionFile: 'Test' } }));
  write(path.join(cfg, 'basic', 'profiles', 'Test', 'basic.ini'), ini({
    General: { Name: 'Test' },
    Video: { BaseCX: cw, BaseCY: ch, OutputCX: ow, OutputCY: oh, FPSType: 0, FPSCommon: fps, ScaleType: 'bicubic' },
    Audio: { SampleRate: rate, ChannelSetup: speakers },
    Output: { Mode: 'Simple' },
    SimpleOutput: { FilePath: recDir.split(path.sep).join('/'), RecFormat2: 'mkv', RecQuality: 'Stream', StreamEncoder: 'x264', RecEncoder: 'x264', Preset: 'ultrafast', VBitrate: 6000, ABitrate: 192 },
  }));
  write(path.join(cfg, 'plugin_config', 'obs-websocket', 'config.json'), JSON.stringify({ server_enabled: true, auth_required: false, server_port: 4466, first_load: false, alerts_enabled: false }));
  write(path.join(cfg, 'plugin_config', 'obs-tab-player', 'settings.json'), JSON.stringify({
    library: library.split(path.sep).join('/'), fileId: 'Sync test.gp5', theme: 'clear',
    speed: 1, loop: true, countIn: false, metronome: false, loopGap: Number(args.loopGap || 0), horizontal: true, dockIntroduced: true,
  }));
  // the tab as a strip across the bottom of the canvas, as "Add Tab Player to this scene" places it
  const k = cw / 1920;
  const item = (name, id, extra = {}) => ({ name, id, visible: true, locked: false, pos: { x: 0, y: 0 }, scale: { x: 1, y: 1 }, rot: 0, align: 5, bounds_type: 0, bounds: { x: 0, y: 0 }, ...extra });
  write(path.join(cfg, 'basic', 'scenes', 'Test.json'), JSON.stringify({
    name: 'Test', current_scene: 'Scene', current_program_scene: 'Scene', scene_order: [{ name: 'Scene' }], groups: [], modules: {},
    sources: [
      { id: 'scene', versioned_id: 'scene', name: 'Scene', enabled: true, flags: 0, mixers: 0, volume: 1, settings: { id_counter: 2, items: [item('Tab Player', 1, { pos: { x: 0, y: ch - 400 * k }, scale: { x: k, y: k } }), item('Tab Player Metronome', 2)] } },
      { id: 'tab_player_source', versioned_id: 'tab_player_source', name: 'Tab Player', enabled: true, flags: 0, mixers: 255, volume: 1, settings: { width: 1920, height: 400 } },
      { id: 'tab_player_metronome', versioned_id: 'tab_player_metronome', name: 'Tab Player Metronome', enabled: true, flags: 0, mixers: 255, volume: 1, settings: {} },
    ],
  }));
}

async function socket() {
  for (let i = 0; i < 80; i++) {
    try {
      const ws = new WebSocket('ws://127.0.0.1:4466');
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
        call: (requestType, requestData) => new Promise((ok) => { const id = String(++n); pending.set(id, ok); ws.send(JSON.stringify({ op: 6, d: { requestType, requestId: id, requestData } })); }),
        close: () => ws.close(),
      };
    } catch { await sleep(500); }
  }
  throw new Error('obs-websocket did not come up');
}

async function record() {
  const obs = spawn(exe, ['--portable', '--multi', '--minimize-to-tray', '--disable-updater', '--disable-shutdown-check', '--disable-missing-files-check'], { cwd: path.dirname(exe), stdio: 'ignore' });
  try {
    const ws = await socket();
    await sleep(3000);
    if (!(await ws.call('StartRecord')).requestStatus?.result) throw new Error('StartRecord failed');
    await sleep(500);
    await ws.call('TriggerHotkeyByName', { hotkeyName: 'tab_player.play_pause' });
    const a = (await ws.call('GetStats')).responseData;
    await sleep(SECONDS * 1000);
    const b = (await ws.call('GetStats')).responseData;
    await ws.call('TriggerHotkeyByName', { hotkeyName: 'tab_player.play_pause' });
    const stop = await ws.call('StopRecord');
    await sleep(800);
    ws.close();
    return { file: stop.responseData?.outputPath, skipped: b.renderSkippedFrames - a.renderSkippedFrames + b.outputSkippedFrames - a.outputSkippedFrames, frames: b.renderTotalFrames - a.renderTotalFrames };
  } finally {
    obs.kill();
    await sleep(2000);
  }
}

// ---- measuring a recording

function pipe(argv, onChunk) {
  return new Promise((ok, fail) => {
    const p = spawn('ffmpeg', ['-v', 'error', ...argv, '-'], { stdio: ['ignore', 'pipe', 'inherit'] });
    p.stdout.on('data', onChunk);
    p.on('close', (code) => (code ? fail(new Error('ffmpeg ' + code)) : ok()));
  });
}

// the strip, scaled back to the 1920x400 the source draws at, is identical in every setting
const W = 1920, H = 400, CURSOR = [220, 320], STEMS = [332, 356], DIGITS = [188, 220];

async function measure(file, fps) {
  // cursor: the column with the most yellow in the cursor band, per frame; stems from one frame
  const cursor = [];
  const litX = []; // per frame: the middle of the lit note's highlight, away from the cursor
  let stems = null, buf = Buffer.alloc(0), frame = 0;
  const size = W * H * 3;
  await pipe(['-i', file, '-vf', `scale=${W}:-2:flags=bicubic,crop=${W}:${H}:0:ih-${H}`, '-f', 'rawvideo', '-pix_fmt', 'rgb24'], (chunk) => {
    buf = Buffer.concat([buf, chunk]);
    while (buf.length >= size) {
      const f = buf.subarray(0, size);
      const col = new Uint16Array(W);
      for (let y = CURSOR[0]; y < CURSOR[1]; y++)
        for (let x = 0; x < W; x++) { const i = (y * W + x) * 3; if (f[i] > 200 && f[i + 1] > 150 && f[i + 2] < 120) col[x]++; }
      // the centre of the cursor line, to a fraction of a pixel
      let best = 0;
      for (let x = 1; x < W; x++) if (col[x] > col[best]) best = x;
      let sum = 0, wsum = 0;
      for (let x = Math.max(0, best - 12); x < Math.min(W, best + 13); x++) if (col[x] > col[best] / 2) { sum += x * col[x]; wsum += col[x]; }
      cursor.push(col[best] > (CURSOR[1] - CURSOR[0]) * 0.5 ? sum / wsum : -1);
      const lit = new Uint16Array(W);
      for (let y = DIGITS[0]; y < DIGITS[1]; y++)
        for (let x = 0; x < W; x++) { const i = (y * W + x) * 3; if (f[i] > 95 && f[i + 1] > 80 && f[i + 2] < 75 && f[i] - f[i + 2] > 50 && Math.abs(x - best) > 10) lit[x]++; }
      let ls = 0, lw = 0;
      for (let x = 0; x < W; x++) if (lit[x] >= 3) { ls += x * lit[x]; lw += lit[x]; }
      litX.push(lw > 40 ? ls / lw : -1);
      if (!stems && frame === Math.round(fps * 3)) {
        const bright = new Uint16Array(W);
        for (let y = STEMS[0]; y < STEMS[1]; y++)
          for (let x = 0; x < W; x++) { const i = (y * W + x) * 3; if (f[i] > 180 && f[i + 1] > 180 && f[i + 2] > 180) bright[x]++; }
        // each stem's centre: the weighted middle of its run of bright columns
        stems = [];
        for (let x = 0; x < W; x++) {
          if (bright[x] < 16) continue;
          let sum = 0, wsum = 0;
          while (x < W && bright[x] >= 8) { sum += x * bright[x]; wsum += bright[x]; x++; }
          stems.push(sum / wsum);
        }
      }
      buf = buf.subarray(size);
      frame++;
    }
  });
  // audio: mono, 48 kHz; a note starts where the envelope crosses a threshold (sub-sample)
  const parts = [];
  await pipe(['-i', file, '-ac', '1', '-ar', '48000', '-f', 's16le'], (c) => parts.push(c));
  const pcm = Buffer.concat(parts);
  const n = pcm.length >> 1, SR = 48000, F = 48; // 1 ms frames
  const x = (i) => pcm.readInt16LE(i * 2) / 32768;
  const rms = new Float32Array(Math.floor(n / F));
  for (let j = 0; j < rms.length; j++) { let a = 0; for (let i = j * F; i < (j + 1) * F; i++) a += x(i) ** 2; rms[j] = Math.sqrt(a / F); }
  // a note starts where the level jumps (to four times the quietest of the last 8 ms), then to the
  // sample: the first one above a quarter of the attack's peak
  const onsets = [];
  for (let j = 8; j < rms.length - 6; j++) {
    let lo = Infinity;
    for (let q = j - 8; q < j; q++) lo = Math.min(lo, rms[q]);
    if (rms[j] < 0.02 || rms[j] < 4 * lo || (onsets.length && j / 1000 - onsets[onsets.length - 1] < 0.08)) continue;
    const from = (j - 2) * F;
    let peak = 0;
    for (let i = from; i < from + 6 * F; i++) peak = Math.max(peak, Math.abs(x(i)));
    let i = from;
    while (i < from + 6 * F && Math.abs(x(i)) < peak / 4) i++;
    onsets.push(i / SR);
    j += 40;
  }
  // lag: where the cursor is when the note sounds, against the note's column, in ms
  // from when the cursor first moves (a note that starts it from rest has no speed to measure)
  const moving = cursor.findIndex((c, i) => i > 0 && c >= 0 && cursor[i - 1] >= 0 && Math.abs(c - cursor[i - 1]) > 0.5);
  const lags = [];
  for (const t of onsets.filter((t) => moving >= 0 && t > moving / fps + 0.3)) {
    const i = Math.floor(t * fps);
    if (i < 1 || i + 1 >= cursor.length || cursor[i] < 0 || cursor[i + 1] < 0) continue;
    const v = (cursor[i + 1] - cursor[i]) * fps; // px per second
    if (v <= 50 || v > 2000) continue; // not moving smoothly (the loop jumps back)
    const x = cursor[i] + (cursor[i + 1] - cursor[i]) * (t * fps - i);
    const note = stems.reduce((a, s) => (Math.abs(s - x) < Math.abs(a - x) ? s : a), stems[0]);
    const ms = ((x - note) / v) * 1000;
    if (Math.abs(ms) < 60) lags.push([t, ms]);
  }
  if (args.debug) console.log({ stems, onsets: onsets.slice(0, 12).map((t) => round(t * 1000) / 1000), lags: lags.slice(0, 12).map(([t, l]) => [round(t * 1000) / 1000, round(l)]) });
  // lit notes: the frame a note's highlight appears, against the moment it sounds (frame times
  // are 1/fps apart, so each lies within half a frame; the average shows the bias)
  const nearest = (x) => stems.reduce((a, s) => (Math.abs(s - x) < Math.abs(a - x) ? s : a), stems[0]);
  const litOn = [];
  for (let i = 1; i < litX.length; i++)
    if (litX[i] >= 0 && (litX[i - 1] < 0 || nearest(litX[i - 1]) !== nearest(litX[i]))) {
      const t = i / fps, near = onsets.filter((o) => Math.abs(o - t) < 0.1);
      if (near.length) litOn.push((t - near[0]) * 1000);
    }
  const litMs = litOn.length ? round(litOn.reduce((a, b) => a + b, 0) / litOn.length) : null;
  const ms = lags.map((l) => l[1]).sort((a, b) => a - b);
  const q = (p) => ms[Math.min(ms.length - 1, Math.floor(p * ms.length))];
  // drift: slope of the lag over the recording
  const mt = lags.reduce((a, l) => a + l[0], 0) / lags.length, ml = lags.reduce((a, l) => a + l[1], 0) / lags.length;
  const slope = lags.reduce((a, l) => a + (l[0] - mt) * (l[1] - ml), 0) / lags.reduce((a, l) => a + (l[0] - mt) ** 2, 0);
  return { litMs, litNotes: litOn.length, notes: lags.length, stems: stems?.length || 0, medianMs: round(q(0.5)), p5: round(q(0.05)), p95: round(q(0.95)), driftMsPerMin: round(slope * 60) };
}
const round = (x) => Math.round(x * 10) / 10;

const rows = [];
for (const m of MATRIX) {
  const name = m[0];
  process.stdout.write(`${name.padEnd(28)}`);
  try {
    configure(m);
    const rec = await record();
    if (!rec.file || !fs.existsSync(rec.file)) throw new Error('no recording');
    const r = await measure(rec.file, FPS[m[5]]);
    const probe = spawnSync('ffprobe', ['-v', 'error', '-show_entries', 'stream=codec_type,width,height,r_frame_rate,sample_rate,channels', '-of', 'compact=p=0:nk=1', rec.file], { encoding: 'utf8' }).stdout.trim().replace(/\r?\n/g, ' | ');
    rows.push({ setting: name, ...r, skippedFrames: rec.skipped, file: probe });
    console.log(`median ${String(r.medianMs).padStart(5)} ms   5-95% ${r.p5}..${r.p95} ms   drift ${r.driftMsPerMin} ms/min   lit ${r.litMs} ms (${r.litNotes})   ${r.notes} notes   skipped ${rec.skipped}/${rec.frames}`);
    if (!args.keep) fs.rmSync(rec.file, { force: true });
  } catch (e) {
    rows.push({ setting: name, error: e.message });
    console.log('ERROR ' + e.message);
  }
}
if (args.json) fs.writeFileSync(args.json, JSON.stringify(rows, null, 1));
