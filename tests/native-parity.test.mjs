// SPDX-License-Identifier: Apache-2.0
// The native (C++) engine must read every file exactly like the JS engine.
// Runs when build/tools/Release/tabtool.exe exists (see plugin/CMakeLists.txt).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { loadScore } from '../player/www/js/core/load.js';
import { buildTimeline } from '../player/www/js/core/timeline.js';
import { layoutTrack } from '../player/www/js/core/render.js';
import { writeGp } from '../tools/gp-writer.mjs';
import { DEMO_SONGS } from '../tools/demo-songs.mjs';
import { gpifFor, makeZip, EFFECTS_SONG, loopingGpx } from './helpers.mjs';
import { buildBcfs, compressBcfz } from './gpx-writer.mjs';

const ROOT = path.join(import.meta.dirname, '..');
const TOOL = process.env.TABTOOL || path.join(ROOT, 'build', 'tools', 'Release', 'tabtool.exe');
const haveTool = fs.existsSync(TOOL);

function project(score) {
  return {
    format: score.format,
    title: score.title,
    tempo: score.tempo,
    notices: score.notices.replace(/\r/g, ''),
    masterBars: score.masterBars.map((m) => ({ num: m.num, den: m.den, repeatStart: m.repeatStart, repeatPlays: m.repeatPlays, alternate: m.alternate, marker: m.marker })),
    tracks: score.tracks.map((t) => ({
      name: t.name, capo: t.capo, percussion: !!t.percussion, tuning: t.tuning,
      bars: t.bars.map((b) => b.voices.map((v) => v.map((x) => ({
        value: x.value, dots: x.dots, tuplet: x.tuplet ? x.tuplet[0] : 0, ticks: x.ticks, start: x.start, rest: !!x.rest,
        chord: x.chord || '', pick: x.pickStroke || '',
        notes: x.notes.map((n) => ({ s: n.string, f: n.fret, tie: n.tie ? 1 : 0, dead: n.dead ? 1 : 0, hammer: n.hammer ? 1 : 0, pm: n.palmMute ? 1 : 0, ring: n.letRing ? 1 : 0, slide: n.slide || '', bend: n.bend ? n.bend.length : 0 })),
      })))),
    })),
  };
}

function native(file, cmd = 'dump') {
  return JSON.parse(execFileSync(TOOL, [cmd, file], { encoding: 'utf8', maxBuffer: 1 << 28 }));
}

function fixtures() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tabparity-'));
  const files = [];
  for (const song of DEMO_SONGS) {
    for (const v of ['3.00', '4.06', '5.00', '5.10']) {
      const f = path.join(dir, `${song.title}.${v}.gp${v[0]}`);
      fs.writeFileSync(f, writeGp(song, v));
      files.push(f);
    }
    const xml = Buffer.from(gpifFor(song));
    const gp = path.join(dir, `${song.title}.gp`);
    fs.writeFileSync(gp, makeZip([['Content/score.gpif', xml]], true));
    const fsys = buildBcfs([['score.gpif', new Uint8Array(xml)]]);
    const gpx = path.join(dir, `${song.title}.gpx`);
    fs.writeFileSync(gpx, compressBcfz(fsys));
    files.push(gp, gpx);
  }
  const fx = path.join(dir, 'effects.gp');
  fs.writeFileSync(fx, makeZip([['Content/score.gpif', Buffer.from(gpifFor(EFFECTS_SONG))]], true));
  files.push(fx);
  return { dir, files };
}

test('native reader matches the JS reader on every format', { skip: !haveTool && 'tabtool not built' }, async () => {
  const { dir, files } = fixtures();
  try {
    for (const f of files) {
      const js = project(await loadScore(fs.readFileSync(f)));
      assert.deepEqual(native(f), js, path.basename(f));
    }
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

async function sameTimeline(f) {
  const tl = buildTimeline(await loadScore(fs.readFileSync(f)));
  const n = native(f, 'timeline');
  assert.ok(Math.abs(n.duration - tl.duration) < 1e-6, `${path.basename(f)} duration`);
  assert.deepEqual(n.order, tl.order);
  assert.equal(n.notes, tl.notes.length, `${path.basename(f)} note count`);
  assert.equal(n.beats, tl.beats.length);
  assert.equal(n.clicks, tl.clicks.length);
  n.first.forEach(([t, dur, midi], i) => {
    assert.ok(Math.abs(t - tl.notes[i].t) < 1e-6 && Math.abs(dur - tl.notes[i].dur) < 1e-6 && midi === tl.notes[i].midi, `${path.basename(f)} note ${i}`);
  });
}

test('native timeline matches the JS timeline', { skip: !haveTool && 'tabtool not built' }, async () => {
  const { dir, files } = fixtures();
  try {
    for (const f of files) await sameTimeline(f);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('optional: native matches JS on every file in TAB_LIBRARY', { skip: (!haveTool || !process.env.TAB_LIBRARY) && 'needs tabtool and TAB_LIBRARY' }, async () => {
  const files = [];
  const walk = (d) => { for (const e of fs.readdirSync(d, { withFileTypes: true })) { const p = path.join(d, e.name); if (e.isDirectory()) walk(p); else if (/\.(gp[345x]?|gp[78])$/i.test(e.name)) files.push(p); } };
  walk(process.env.TAB_LIBRARY);
  const diffs = [];
  for (const f of files) {
    try {
      assert.deepEqual(native(f), project(await loadScore(fs.readFileSync(f))));
      await sameTimeline(f);
    } catch (e) { diffs.push(path.basename(f) + ': ' + String(e.message).split('\n')[0]); }
  }
  assert.deepEqual(diffs.slice(0, 10), []);
  console.log(`  ${files.length} files identical`);
});

// The browser renderer is a port of the plugin's layout: every primitive must match.
function sameLayout(score, f, width, horizontal, scale) {
  const n = JSON.parse(execFileSync(TOOL, ['layout', f, String(width), horizontal ? '1' : '0', String(scale)], { encoding: 'utf8', maxBuffer: 1 << 28 }));
  const js = layoutTrack(score, 0, { width, mode: horizontal ? 'horizontal' : 'page', scale });
  const name = `${path.basename(f)} ${horizontal ? 'line' : 'page'} ${width}`;
  assert.equal(js.width, n.width, `${name} width`);
  assert.equal(js.height, n.height, `${name} height`);
  assert.equal(js.prims.length, n.prims.length, `${name} primitive count`);
  const close = (a, b) => Math.abs(a - b) < 0.03;
  js.prims.forEach((p, i) => {
    const [type, role, x0, y0, x1, y1, size, align, bold, italic, string, beatKey, text, d, stroke] = n.prims[i];
    const what = `${name} primitive ${i} (${text || d || type})`;
    assert.ok(p.type === type && p.role === role, `${what}: type/role ${p.type}/${p.role} vs ${type}/${role}`);
    assert.ok(close(p.x0, x0) && close(p.y0, y0) && close(p.x1, x1) && close(p.y1, y1) && close(p.size, size) && close(p.stroke, stroke), `${what}: geometry`);
    assert.ok(p.align === align && +p.bold === bold && +p.italic === italic && p.string === string && p.beatKey === beatKey && p.text === text, `${what}: attributes`);
    const nums = d.split(/[ A-Z]+/).filter(Boolean).map(Number);
    const mine = p.path.flatMap((op) => op.slice(1));
    assert.equal(p.path.map((op) => op[0]).join(''), d.replace(/[^A-Z]/g, ''), `${what}: path ops`);
    assert.ok(mine.length === nums.length && mine.every((v, k) => close(v, nums[k])), `${what}: path`);
  });
}

test('browser renderer draws exactly what the plugin draws', { skip: !haveTool && 'tabtool not built' }, async () => {
  const { dir, files } = fixtures();
  try {
    for (const f of files) {
      const score = await loadScore(fs.readFileSync(f));
      for (const [width, horizontal, scale] of [[900, false, 1], [1280, false, 1.4], [900, true, 1], [600, false, 0.8]]) sameLayout(score, f, width, horizontal, scale);
    }
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});

test('optional: browser renderer matches the plugin on every file in TAB_LIBRARY', { skip: (!haveTool || !process.env.TAB_LIBRARY) && 'needs tabtool and TAB_LIBRARY' }, async () => {
  const files = [];
  const walk = (d) => { for (const e of fs.readdirSync(d, { withFileTypes: true })) { const p = path.join(d, e.name); if (e.isDirectory()) walk(p); else if (/.(gp[345x]?|gp[78])$/i.test(e.name)) files.push(p); } };
  walk(process.env.TAB_LIBRARY);
  for (const f of files) {
    const score = await loadScore(fs.readFileSync(f));
    sameLayout(score, f, 1100, false, 1.2);
    sameLayout(score, f, 900, true, 1);
  }
  console.log(`  ${files.length} files drawn identically`);
});

test('native reader rejects a looping GPX container quickly', { skip: !haveTool && 'tabtool not built' }, () => {
  const f = path.join(os.tmpdir(), `tabloop-${process.pid}.gpx`);
  fs.writeFileSync(f, loopingGpx());
  try {
    assert.throws(() => execFileSync(TOOL, ['dump', f], { encoding: 'utf8', timeout: 10000, stdio: 'pipe' }), (e) => e.status === 1 && !e.signal); // rejected, not timed out
  } finally {
    fs.rmSync(f, { force: true });
  }
});
