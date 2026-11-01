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
import { writeGp } from '../tools/gp-writer.mjs';
import { DEMO_SONGS } from '../tools/demo-songs.mjs';
import { gpifFor, makeZip } from './helpers.mjs';
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

test('native timeline matches the JS timeline', { skip: !haveTool && 'tabtool not built' }, async () => {
  const { dir, files } = fixtures();
  try {
    for (const f of files) {
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
    try { assert.deepEqual(native(f), project(await loadScore(fs.readFileSync(f)))); } catch (e) { diffs.push(path.basename(f) + ': ' + String(e.message).split('\n')[0]); }
  }
  assert.deepEqual(diffs.slice(0, 10), []);
  console.log(`  ${files.length} files identical`);
});
