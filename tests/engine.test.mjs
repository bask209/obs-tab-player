// SPDX-License-Identifier: Apache-2.0
// Timeline, renderer and synthesizer tests.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadScore } from '../player/www/js/core/load.js';
import { playOrder, buildTimeline, loopRange, countInClicks } from '../player/www/js/core/timeline.js';
import { renderTrack } from '../player/www/js/core/render.js';
import { writeGp } from '../tools/gp-writer.mjs';
import { DEMO_SONGS, STANDARD } from '../tools/demo-songs.mjs';
import { gpifFor, makeZip } from './helpers.mjs';

const mb = (o = {}) => ({ num: 4, den: 4, repeatStart: false, repeatPlays: 0, alternate: 0, marker: '', ...o });
const scoreOf = async (song) => loadScore(writeGp(song, '5.10'));

test('repeats and alternate endings expand to the right bar order', () => {
  assert.deepEqual(playOrder([mb(), mb(), mb()]), [0, 1, 2]);
  assert.deepEqual(playOrder([mb({ repeatStart: true }), mb(), mb({ alternate: 1, repeatPlays: 2 }), mb({ alternate: 2 }), mb()]), [0, 1, 2, 0, 1, 3, 4]);
  assert.deepEqual(playOrder([mb(), mb({ repeatPlays: 3 }), mb({ repeatStart: true }), mb({ repeatPlays: 2 })]), [0, 1, 0, 1, 0, 1, 2, 3, 2, 3]);
  // endings 1+2 share a bar, 3 is the last time
  assert.deepEqual(playOrder([mb({ repeatStart: true }), mb({ alternate: 0b011, repeatPlays: 3 }), mb({ alternate: 0b100 })]), [0, 1, 0, 1, 0, 2]);
});

test('timeline timing follows tempo, meter and repeats', async () => {
  const s = await scoreOf(DEMO_SONGS[0]); // 90 BPM, 4 bars, all repeated once
  const tl = buildTimeline(s);
  assert.deepEqual(tl.order, [0, 1, 2, 3, 0, 1, 2, 3]);
  const bar = (4 * 60) / 90;
  assert.ok(Math.abs(tl.duration - 8 * bar) < 1e-9);
  // eighth notes, evenly spaced
  const first = tl.notes.filter((n) => n.t < bar);
  assert.equal(first.length, 8);
  first.forEach((n, i) => assert.ok(Math.abs(n.t - (i * bar) / 8) < 1e-9));
  assert.equal(tl.clicks.length, 8 * 4);
  assert.equal(tl.beats.length, tl.order.reduce((a, i) => a + s.tracks[0].bars[i].voices[0].length, 0));
});

test('triplets, alternate endings and loop ranges', async () => {
  const legato = buildTimeline(await scoreOf(DEMO_SONGS[1]));
  const sec = 60 / 70 / 3; // eighth triplet at 70 BPM
  assert.ok(Math.abs(legato.notes[1].t - legato.notes[0].t - sec) < 1e-9);
  const rhythm = buildTimeline(await scoreOf(DEMO_SONGS[3]));
  assert.deepEqual(rhythm.order, [0, 1, 0, 2]);
  const [a, b] = loopRange(rhythm, 2, 2);
  assert.equal(a, rhythm.bars[1].start);
  assert.equal(b, rhythm.bars[1].end);
  const ci = countInClicks(await scoreOf(DEMO_SONGS[3]), rhythm, 0);
  assert.equal(ci.clicks.length, 4);
  assert.ok(Math.abs(ci.dur - (4 * 60) / 110) < 1e-9);
});

test('ties extend notes, let ring sustains, hammer-ons are legato, bends carried', async () => {
  const song = {
    title: 't', tempo: 60, masterBars: [mb()],
    tracks: [{ name: 'g', tuning: STANDARD, bars: [{ voices: [[
      { value: 4, notes: [{ string: 0, fret: 5 }] },
      { value: 4, notes: [{ string: 0, fret: 5, tie: true }] },
      { value: 4, notes: [{ string: 1, fret: 5, hammer: true, letRing: true }] },
      { value: 4, notes: [{ string: 1, fret: 7, bend: [{ t: 0, semis: 0 }, { t: 1, semis: 2 }] }] },
    ]] }] }],
  };
  const tl = buildTimeline(await scoreOf(song));
  assert.equal(tl.notes.length, 3);
  assert.equal(tl.notes[0].midi, 69);
  assert.ok(Math.abs(tl.notes[0].dur - 2) < 1e-9, 'tie adds the second quarter');
  assert.equal(tl.notes[2].legato, true);
  assert.deepEqual(tl.notes[2].bend.map((p) => p.semis), [0, 2]);
  assert.deepEqual(tl.notes[2].bend.map((p) => p.t), [0, 1], 'bend times are seconds from the note start');
});

test('a bend carries on through tied notes: bend up, then release on the tie', async () => {
  const song = {
    title: 't', tempo: 60, masterBars: [mb()],
    tracks: [{ name: 'g', tuning: STANDARD, bars: [{ voices: [[
      { value: 4, notes: [{ string: 1, fret: 8, bend: [{ t: 0, semis: 0 }, { t: 1, semis: 2 }] }] },
      { value: 4, notes: [{ string: 1, fret: 8, tie: true, bend: [{ t: 0, semis: 2 }, { t: 1, semis: 0 }] }] },
      { value: 2, notes: [{ string: 0, fret: 5 }] },
    ]] }] }],
  };
  const tl = buildTimeline(await scoreOf(song));
  const bent = tl.notes[0];
  assert.ok(Math.abs(bent.dur - 2) < 1e-9);
  // up over the first second, back down over the second one
  assert.deepEqual(bent.bend.map((p) => [p.t, p.semis]), [[0, 0], [1, 2], [1, 2], [2, 0]]);
});

test('swing (triplet feel) plays pairs of 8ths as 2/3 + 1/3 of the beat', async () => {
  const eighths = Array.from({ length: 8 }, (_, i) => ({ value: 8, notes: [{ string: 0, fret: i }] }));
  const song = { title: 't', tempo: 60, masterBars: [mb()], tracks: [{ name: 'g', tuning: STANDARD, bars: [{ voices: [eighths] }] }] };
  const s = await scoreOf(song);
  s.masterBars[0].tripletFeel = 1; // as read from a GP5 bar
  const tl = buildTimeline(s);
  const starts = tl.notes.map((n) => Math.round(n.t * 1000) / 1000);
  assert.deepEqual(starts, [0, 0.667, 1, 1.667, 2, 2.667, 3, 3.667]);
  assert.ok(Math.abs(tl.notes[0].dur - 2 / 3) < 1e-9);
  assert.equal(tl.beats[1].t, tl.notes[1].t, 'the cursor swings too');
  s.masterBars[0].tripletFeel = 0;
  assert.deepEqual(buildTimeline(s).notes.map((n) => n.t), [0, 0.5, 1, 1.5, 2, 2.5, 3, 3.5]);
});

test('Guitar Pro 7 grace beats become grace notes and take no time in the bar', async () => {
  const q = (fret, o = {}) => ({ value: 4, notes: [{ string: 2, fret, ...o }] });
  const song = { title: 't', artist: '', tempo: 60, masterBars: [mb()], tracks: [{ name: 'g', tuning: STANDARD, bars: [{ voices: [[q(5), { value: 32, notes: [{ string: 2, fret: 7, hammer: true }] }, q(9), q(9), q(9)]] }] }] };
  // the second beat is the grace beat, as Guitar Pro 7 writes it
  const xml = gpifFor(song).replace('<Beat id="1">', '<Beat id="1"><GraceNotes>BeforeBeat</GraceNotes>');
  const s = await loadScore(makeZip([['Content/score.gpif', Buffer.from(xml)]]));
  const beats = s.tracks[0].bars[0].voices[0];
  assert.equal(beats.length, 4);
  assert.deepEqual(beats[1].notes[0].grace, { fret: 7, hammer: true });
  const starts = buildTimeline(s).notes.map((n) => Math.round(n.t * 1000) / 1000);
  assert.deepEqual(starts, [0, 0.94, 1, 2, 3]);
});

test('renderer: every demo song, both layouts, several sizes, no NaN, full cursor geometry', async () => {
  for (const song of DEMO_SONGS) {
    const s = await scoreOf(song);
    for (const mode of ['page', 'horizontal']) {
      for (const width of [360, 960, 1920]) {
        for (const scale of [0.7, 1.2, 2]) {
          const r = renderTrack(s, 0, { width, mode, scale });
          assert.ok(!r.svg.includes('NaN') && !r.svg.includes('undefined'), `${song.title} ${mode} ${width} ${scale}`);
          assert.equal(r.bars.filter(Boolean).length, s.masterBars.length);
          for (const [bi, bar] of s.tracks[0].bars.entries()) {
            bar.voices[0].forEach((_, k) => assert.ok(Number.isFinite(r.beats.get(bi * 4096 + k)), 'beat x'));
          }
          if (mode === 'page') {
            // rows fit the width unless a single bar is wider than the whole overlay
            r.systems.forEach((sys) => {
              const bars = r.bars.filter((b) => b && r.systems[b.sys] === sys);
              if (bars.length > 1) assert.ok(sys.x1 <= width + 1, `${song.title} row overflows at ${width}px x${scale}`);
            });
          }
        }
      }
    }
  }
});

test('renderer escapes text from files', async () => {
  const song = { ...DEMO_SONGS[4], masterBars: DEMO_SONGS[4].masterBars.map((m, i) => (i === 0 ? { ...m, marker: '<script>&' } : m)) };
  const r = renderTrack(await scoreOf(song), 0, {});
  assert.ok(!r.svg.includes('<script>'));
  assert.ok(r.svg.includes('&lt;script&gt;&amp;'));
});

test('synth string model is in tune across the neck', async () => {
  globalThis.window = globalThis;
  const { Synth } = await import('../player/www/js/core/synth.js');
  const fake = { createBuffer: (ch, len, sr) => { const d = new Float32Array(len); return { getChannelData: () => d, duration: len / sr, sampleRate: sr }; } };
  const s = new Synth();
  s.ctx = fake;
  for (const midi of [40, 45, 52, 59, 64, 71, 76, 84]) {
    const buf = s.buffer(midi, 'pluck');
    const d = buf.getChannelData(0);
    const sr = buf.sampleRate;
    const f = 440 * 2 ** ((midi - 69) / 12);
    // autocorrelation peak near the expected period, refined by parabolic interpolation
    const start = Math.round(sr * 0.2);
    const n = Math.round(sr * 0.25);
    const expected = sr / f;
    let best = 0; let bestLag = 0;
    const corr = (lag) => { let c = 0; for (let i = start; i < start + n; i++) c += d[i] * d[i + lag]; return c; };
    for (let lag = Math.floor(expected * 0.9); lag <= Math.ceil(expected * 1.1); lag++) { const c = corr(lag); if (c > best) { best = c; bestLag = lag; } }
    const a = corr(bestLag - 1), b = corr(bestLag), c = corr(bestLag + 1);
    const period = bestLag + (0.5 * (a - c)) / (a - 2 * b + c);
    const cents = 1200 * Math.log2(expected / period);
    assert.ok(Math.abs(cents) < 5, `midi ${midi} off by ${cents.toFixed(1)} cents`);
    assert.ok(Math.max(...d.subarray(0, 4000).map(Math.abs)) > 0.1, 'audible');
  }
});
