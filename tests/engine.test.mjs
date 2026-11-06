// SPDX-License-Identifier: Apache-2.0
// Timeline, layout and synthesizer tests, run on the engine itself (see core.mjs).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { core, open, loadScore, timelineOf } from './core.mjs';
import { layoutSvg } from '../build/web/core.mjs';
import { writeGp } from '../tools/gp-writer.mjs';
import { DEMO_SONGS, STANDARD } from '../tools/demo-songs.mjs';
import { gpifFor, makeZip, EFFECTS_SONG } from './helpers.mjs';

const mb = (o = {}) => ({ num: 4, den: 4, ...o });
const gp5 = (song) => writeGp(song, '5.10');
const gp7 = (song) => makeZip([['Content/score.gpif', Buffer.from(gpifFor(song))]]);
const song = (masterBars, bars, tempo = 60) => ({ title: 't', artist: '', tempo, masterBars, tracks: [{ name: 'g', tuning: STANDARD, bars }] });
const whole = (fret) => ({ voices: [[{ value: 1, notes: [{ string: 0, fret }] }]] });
// the bar order the engine plays, for these bars (Guitar Pro 7 file)
const orderOf = (masterBars) => timelineOf(gp7(song(masterBars, masterBars.map((_, i) => whole(i))))).order;

test('repeats and alternate endings expand to the right bar order', () => {
  assert.deepEqual(orderOf([mb(), mb(), mb()]), [0, 1, 2]);
  assert.deepEqual(orderOf([mb({ repeatStart: true }), mb(), mb({ alternate: 1, repeatPlays: 2 }), mb({ alternate: 2 }), mb()]), [0, 1, 2, 0, 1, 3, 4]);
  assert.deepEqual(orderOf([mb(), mb({ repeatPlays: 3 }), mb({ repeatStart: true }), mb({ repeatPlays: 2 })]), [0, 1, 0, 1, 0, 1, 2, 3, 2, 3]);
  // endings 1+2 share a bar, 3 is the last time
  assert.deepEqual(orderOf([mb({ repeatStart: true }), mb({ alternate: [1, 2], repeatPlays: 3 }), mb({ alternate: 3 })]), [0, 1, 0, 1, 0, 2]);
});

test('D.C., D.S., coda and fine jumps are followed', () => {
  assert.deepEqual(orderOf([mb({ targets: ['Segno'] }), mb({ jump: 'DaCoda' }), mb({ jump: 'DaSegnoAlCoda' }), mb({ targets: ['Coda'] })]), [0, 1, 2, 0, 1, 3]);
  // D.C. al Fine stops at the fine
  assert.deepEqual(orderOf([mb(), mb({ targets: ['Fine'] }), mb({ jump: 'DaCapoAlFine' }), mb()]), [0, 1, 2, 0, 1]);
  // after the D.C. the repeat is not played again, and only the last ending
  assert.deepEqual(orderOf([mb({ repeatStart: true }), mb({ alternate: 1, repeatPlays: 2 }), mb({ alternate: 2, jump: 'DaCapo' }), mb()]), [0, 1, 0, 2, 0, 2, 3]);
  // To Coda does nothing before the jump back
  assert.deepEqual(orderOf([mb({ jump: 'DaCoda' }), mb({ jump: 'DaCapoAlCoda' }), mb({ targets: ['Coda'] })]), [0, 1, 0, 2]);
});

test('natural harmonics sound at the partial of the node they touch', () => {
  const at = [12, 7, 5, 4, 9, 3.2, 2.7, 2.4, 19, 24];
  const bars = at.map((hf) => ({ voices: [[{ value: 1, notes: [{ string: 0, fret: Math.round(hf), harmonic: 'natural', harmonicFret: hf }] }]] }));
  const notes = timelineOf(gp7(song(at.map(() => mb()), bars))).notes;
  assert.deepEqual(notes.map((n) => n.midi - 64), [12, 19, 24, 28, 28, 31, 34, 36, 19, 24]);
});

test('whammy, trills, tapped harmonics, fermatas and grace notes play like Guitar Pro', () => {
  const tl = timelineOf(gp7(EFFECTS_SONG));
  assert.deepEqual(tl.order, [0, 1, 2, 0, 1, 3]);
  const beat = 60 / 90;
  const at = (t) => tl.notes.filter((n) => Math.abs(n.t - t) < 1e-6);
  // the whammy dive moves the note on beat 3 down a tone and back
  assert.deepEqual(at(2 * beat)[0].bend.map((p) => p.semis), [0, -2, 0]);
  // the trill alternates 5 and 7 on the G string in 16ths, slurred
  const trill = tl.notes.filter((n) => n.t >= 3 * beat - 1e-6 && n.t < 4 * beat - 1e-6);
  assert.deepEqual(trill.map((n) => n.midi), [60, 62, 60, 62]);
  assert.ok(trill.slice(1).every((n) => n.legato));
  // a harmonic tapped 7 frets above fret 5 sounds a twelfth above it; one touched at 3.2 sounds partial 6
  assert.equal(at(4 * beat)[0].midi, 55 + 5 + 19);
  assert.equal(at(5 * beat)[0].midi, 55 + 31);
  // the fermata holds beat 4 of bar 2 half as long again
  assert.ok(Math.abs(tl.bars[1].end - tl.bars[1].start - 4.5 * beat) < 1e-9);
});

test('timeline timing follows tempo, meter and repeats', () => {
  const { score, timeline: tl } = open(gp5(DEMO_SONGS[0])); // 90 BPM, 4 bars, all repeated once
  assert.deepEqual(tl.order, [0, 1, 2, 3, 0, 1, 2, 3]);
  const bar = (4 * 60) / 90;
  assert.ok(Math.abs(tl.duration - 8 * bar) < 1e-9);
  // eighth notes, evenly spaced
  const first = tl.notes.filter((n) => n.t < bar);
  assert.equal(first.length, 8);
  first.forEach((n, i) => assert.ok(Math.abs(n.t - (i * bar) / 8) < 1e-9));
  assert.equal(tl.clicks.length, 8 * 4);
  assert.equal(tl.beats.length, tl.order.reduce((a, i) => a + score.tracks[0].bars[i].voices[0].length, 0));
});

test('triplets and alternate endings', () => {
  const legato = timelineOf(gp5(DEMO_SONGS[1]));
  const sec = 60 / 70 / 3; // eighth triplet at 70 BPM
  assert.ok(Math.abs(legato.notes[1].t - legato.notes[0].t - sec) < 1e-9);
  assert.deepEqual(timelineOf(gp5(DEMO_SONGS[3])).order, [0, 1, 0, 2]);
});

test('ties extend notes, let ring sustains, hammer-ons are legato, bends carried', () => {
  const tl = timelineOf(gp5(song([mb()], [{ voices: [[
    { value: 4, notes: [{ string: 0, fret: 5 }] },
    { value: 4, notes: [{ string: 0, fret: 5, tie: true }] },
    { value: 4, notes: [{ string: 1, fret: 5, hammer: true, letRing: true }] },
    { value: 4, notes: [{ string: 1, fret: 7, bend: [{ t: 0, semis: 0 }, { t: 1, semis: 2 }] }] },
  ]] }])));
  assert.equal(tl.notes.length, 3);
  assert.equal(tl.notes[0].midi, 69);
  assert.ok(Math.abs(tl.notes[0].dur - 2) < 1e-9, 'tie adds the second quarter');
  assert.equal(tl.notes[2].legato, true);
  assert.deepEqual(tl.notes[2].bend.map((p) => p.semis), [0, 2]);
  assert.deepEqual(tl.notes[2].bend.map((p) => p.t), [0, 1], 'bend times are seconds from the note start');
});

test('a bend carries on through tied notes: bend up, then release on the tie', () => {
  const tl = timelineOf(gp5(song([mb()], [{ voices: [[
    { value: 4, notes: [{ string: 1, fret: 8, bend: [{ t: 0, semis: 0 }, { t: 1, semis: 2 }] }] },
    { value: 4, notes: [{ string: 1, fret: 8, tie: true, bend: [{ t: 0, semis: 2 }, { t: 1, semis: 0 }] }] },
    { value: 2, notes: [{ string: 0, fret: 5 }] },
  ]] }])));
  const bent = tl.notes[0];
  assert.ok(Math.abs(bent.dur - 2) < 1e-9);
  // up over the first second, back down over the second one
  assert.deepEqual(bent.bend.map((p) => [p.t, p.semis]), [[0, 0], [1, 2], [1, 2], [2, 0]]);
});

test('swing (triplet feel) plays pairs of 8ths as 2/3 + 1/3 of the beat', () => {
  const eighths = { voices: [Array.from({ length: 8 }, (_, i) => ({ value: 8, notes: [{ string: 0, fret: i }] }))] };
  for (const write of [gp5, gp7]) { // Guitar Pro 5 stores swing per bar, Guitar Pro 7 by name
    const tl = timelineOf(write(song([mb({ feel: 'Triplet8th' })], [eighths])));
    assert.deepEqual(tl.notes.map((n) => Math.round(n.t * 1000) / 1000), [0, 0.667, 1, 1.667, 2, 2.667, 3, 3.667]);
    assert.ok(Math.abs(tl.notes[0].dur - 2 / 3) < 1e-9);
    assert.equal(tl.beats[1].t, tl.notes[1].t, 'the cursor swings too');
  }
  assert.deepEqual(timelineOf(gp5(song([mb()], [eighths]))).notes.map((n) => n.t), [0, 0.5, 1, 1.5, 2, 2.5, 3, 3.5]);
});

test('Guitar Pro 7 grace beats become grace notes and take no time in the bar', () => {
  const q = (fret, o = {}) => ({ value: 4, notes: [{ string: 2, fret, ...o }] });
  const s = song([mb()], [{ voices: [[q(5), { value: 32, grace: 'BeforeBeat', notes: [{ string: 2, fret: 7, hammer: true }] }, q(9), q(9), q(9)]] }]);
  const { score, timeline } = open(gp7(s));
  const beats = score.tracks[0].bars[0].voices[0];
  assert.equal(beats.length, 4);
  assert.deepEqual(beats[1].notes[0].grace, { fret: 7, hammer: true });
  assert.deepEqual(timeline.notes.map((n) => Math.round(n.t * 1000) / 1000), [0, 0.94, 1, 2, 3]);
});

test('a band file opens on its first pitched track', () => {
  const s = song([mb()], [whole(3)]);
  s.tracks = [{ name: 'Drums', tuning: STANDARD, percussion: true, bars: [{ voices: [[{ value: 1, rest: true, notes: [] }]] }] }, ...s.tracks];
  const info = core.load(gp5(s));
  assert.equal(info.track, 1);
  assert.deepEqual(info.tracks.map((t) => t.percussion), [true, false]);
});

test('layout: every demo song, both layouts, several sizes, finite, full cursor geometry', () => {
  for (const s of DEMO_SONGS) {
    const { score } = open(gp5(s));
    for (const horizontal of [false, true]) {
      for (const width of [360, 960, 1920]) {
        for (const scale of [0.7, 1.2, 2]) {
          const l = core.layout({ width, horizontal, scale });
          const what = `${s.title} ${horizontal ? 'line' : 'page'} ${width} x${scale}`;
          assert.ok(l.prims.every((p) => [p.x0, p.y0, p.x1, p.y1].every((v) => Number.isFinite(v) && Math.abs(v) < 1e6)), what);
          assert.equal(l.bars.length, score.masterBars.length);
          for (const [bi, bar] of score.tracks[0].bars.entries()) bar.voices[0].forEach((_, k) => assert.ok(Number.isFinite(l.beats.get(bi * 4096 + k)), `${what} beat x`));
          if (!horizontal) {
            // rows fit the width unless a single bar is wider than the whole view
            l.systems.forEach((sys, si) => {
              if (l.bars.filter((b) => b.sys === si).length > 1) assert.ok(sys.x1 <= width + 1, `${what}: row overflows`);
            });
          }
        }
      }
    }
  }
});

test('the SVG escapes text from files', () => {
  const s = { ...DEMO_SONGS[4], masterBars: DEMO_SONGS[4].masterBars.map((m, i) => (i === 0 ? { ...m, marker: '<script>&' } : m)) };
  open(gp5(s));
  const svg = layoutSvg(core.layout({}), 1);
  assert.ok(!svg.includes('<script>'));
  assert.ok(svg.includes('&lt;script&gt;&amp;'));
});

test('synth: in tune across the neck, clicks on their own output', () => {
  // one long note per pitch, rendered by the plugin's synth; autocorrelation finds the period
  for (const [string, fret] of [[5, 0], [4, 0], [3, 2], [1, 0], [0, 0], [0, 7], [0, 12], [0, 20]]) {
    core.load(gp5(song([mb()], [{ voices: [[{ value: 1, notes: [{ string, fret }] }]] }])), 48000);
    core.play(0, 1, false, false);
    const { part, clicks } = core.render(48000);
    const midi = STANDARD[string] + fret;
    const sr = 48000;
    const expected = sr / (440 * 2 ** ((midi - 69) / 12));
    const start = Math.round(sr * 0.2);
    const n = Math.round(sr * 0.25);
    const corr = (lag) => { let c = 0; for (let i = start; i < start + n; i++) c += part[i] * part[i + lag]; return c; };
    let best = 0; let bestLag = 0;
    for (let lag = Math.floor(expected * 0.9); lag <= Math.ceil(expected * 1.1); lag++) { const c = corr(lag); if (c > best) { best = c; bestLag = lag; } }
    const a = corr(bestLag - 1); const b = corr(bestLag); const c = corr(bestLag + 1);
    const period = bestLag + (0.5 * (a - c)) / (a - 2 * b + c);
    const cents = 1200 * Math.log2(expected / period);
    assert.ok(Math.abs(cents) < 5, `midi ${midi} off by ${cents.toFixed(1)} cents`);
    assert.ok(part.some((v) => Math.abs(v) > 0.05), 'audible');
    assert.ok(clicks.every((v) => v === 0), 'no clicks without the metronome');
  }
  core.setMetronome(true);
  core.play(0, 1, false, false);
  const { clicks } = core.render(4800);
  assert.ok(clicks.some((v) => Math.abs(v) > 0.05), 'the metronome plays on the click output');
});

// ---- count-ins and the bar between loop repeats

const SR = 48000;
const eighths = (n, fret = 3) => ({ voices: [Array.from({ length: n }, () => ({ value: 8, notes: [{ string: 0, fret }] }))] });
// renders `seconds` of audio in 10 ms steps: the count shown, the song position and the loudest sample
function trace(seconds) {
  const out = [];
  for (let i = 0; i < Math.round((seconds * SR) / 480); i++) {
    const at = core.rendered;
    const { part, clicks } = core.render(480);
    let peak = 0; let click = 0;
    for (let k = 0; k < 480; k++) { peak = Math.max(peak, Math.abs(part[k])); click = Math.max(click, Math.abs(clicks[k])); }
    out.push({ t: at / SR, count: core.countInAt(at), pos: core.positionAt(at), peak, click });
  }
  return out;
}
const counting = (tr) => tr.filter((s) => s.count > 0);

test('count-in: one bar of the meter, counted in the beats you would tap', () => {
  // [num, den, bpm, beats counted]: dotted beats in compound meters, never more than five a second
  const cases = [[4, 4, 120, 4], [3, 4, 120, 3], [2, 2, 120, 2], [1, 4, 120, 1], [5, 4, 120, 5], [7, 8, 120, 7], [6, 8, 120, 2], [12, 8, 120, 4],
    [6, 16, 120, 2], [12, 16, 120, 4], [15, 16, 120, 5], [16, 16, 120, 8], [64, 64, 120, 8], [9, 8, 240, 3]];
  for (const [num, den, bpm, beats] of cases) {
    core.load(gp5(song([mb({ num, den })], [eighths(1)], bpm)), SR);
    core.setLoopGap('none');
    core.play(0, 1, true, false);
    const bar = (4 * num / den) * 60 / bpm;
    const c = counting(trace(bar + 0.3));
    const label = `${num}/${den} at ${bpm}`;
    assert.equal(Math.max(...c.map((s) => s.count)), beats, `${label}: beats`);
    assert.ok(Math.abs(c.length * 0.01 - bar) < 0.015, `${label}: lasts one bar (${(c.length * 0.01).toFixed(2)} s, not ${bar})`);
    // every beat gets an equal share of the bar, in order
    for (let b = 1; b <= beats; b++) {
      const n = c.filter((s) => s.count === b).length * 0.01;
      assert.ok(Math.abs(n - bar / beats) < 0.015, `${label}: beat ${b} lasts ${n.toFixed(2)} s`);
    }
  }
  // at half speed the count-in takes twice as long
  core.load(gp5(song([mb()], [eighths(1)], 120)), SR);
  core.play(0, 0.5, true, false);
  assert.ok(Math.abs(counting(trace(4.3)).length * 0.01 - 4) < 0.015);
});

test('loop gap: counted in the meter of the loop\'s own first bar, then the loop plays on', () => {
  // 4/4, then the looped 7/8 bar, then 3/4: the gap must count 7, not the 4 of the first bar
  const file = gp5(song([mb(), mb({ num: 7, den: 8 }), mb({ num: 3, den: 4 })], [eighths(8), eighths(7, 5), eighths(6)], 120));
  for (const mode of ['rest', 'count']) {
    core.load(file, SR);
    const b1 = core.timeline().bars[1];
    core.setLoopGap(mode);
    core.setLoop(true, b1.start, b1.end);
    core.play(b1.start, 1, false, true);
    const len = b1.end - b1.start; // 1.75 s
    const tr = trace(len + 1.75 + 0.6);
    const gap = counting(tr);
    assert.equal(Math.max(...gap.map((s) => s.count)), 7, `${mode}: counts 7`);
    assert.ok(Math.abs(gap.length * 0.01 - 1.75) < 0.015, `${mode}: one 7/8 bar (${(gap.length * 0.01).toFixed(2)} s)`);
    assert.ok(Math.abs(gap[0].t - (0.03 + len)) < 0.015, `${mode}: starts when the loop ends`);
    assert.ok(gap.every((s) => Math.abs(s.pos - b1.start) < 1e-9), `${mode}: the cursor waits at the loop start`);
    // silent guitar once the last note has faded; clicks only when asked for
    assert.ok(gap.slice(20).every((s) => s.peak < 0.01), `${mode}: no guitar during the gap`);
    assert.equal(gap.some((s) => s.click > 0.05), mode === 'count', `${mode}: clicks`);
    // then the loop plays again from its start, on time
    const after = tr.filter((s) => s.t >= gap[gap.length - 1].t + 0.01);
    assert.ok(after[0].peak > 0.05, `${mode}: the first note sounds right after the gap`);
    assert.ok(Math.abs(after[5].pos - (b1.start + (after[5].t - after[0].t))) < 0.011, `${mode}: then plays at speed`);
  }
  core.setLoopGap('none');
});
