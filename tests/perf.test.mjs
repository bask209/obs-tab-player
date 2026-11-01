// SPDX-License-Identifier: Apache-2.0
// Performance budgets on a deliberately huge score (400 bars of 16th-note chords),
// far bigger than a typical lesson. Budgets are generous so slow CI machines pass;
// the measured numbers are printed for comparison.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { loadScore } from '../player/www/js/core/load.js';
import { buildTimeline } from '../player/www/js/core/timeline.js';
import { renderTrack } from '../player/www/js/core/render.js';
import { writeGp } from '../tools/gp-writer.mjs';
import { STANDARD } from '../tools/demo-songs.mjs';

function hugeSong(bars = 400) {
  const masterBars = Array.from({ length: bars }, (_, i) => ({ num: 4, den: 4, repeatStart: i % 8 === 0, repeatPlays: i % 8 === 7 ? 2 : 0 }));
  const bar = (i) => ({ voices: [Array.from({ length: 16 }, (_, k) => ({
    value: 16, pickStroke: k % 2 ? 'up' : 'down', chord: k === 0 ? 'Am' : undefined,
    notes: [{ string: k % 6, fret: (i + k) % 15, hammer: k % 4 === 1, palmMute: k % 8 > 5 }, { string: (k + 2) % 6, fret: (i * 3 + k) % 12 }].sort((a, b) => a.string - b.string)
      .filter((n, j, all) => j === 0 || n.string !== all[j - 1].string),
  }))] });
  return { title: 'Huge', tempo: 140, masterBars, tracks: [{ name: 'g', tuning: STANDARD, bars: masterBars.map((_, i) => bar(i)) }] };
}

const time = (fn) => { const t = performance.now(); const r = fn(); return [r, performance.now() - t]; };

test('huge score: parse, timeline and render stay fast', async () => {
  const bytes = writeGp(hugeSong(), '5.10');
  const t0 = performance.now();
  const score = await loadScore(bytes);
  const parseMs = performance.now() - t0;
  const [tl, timelineMs] = time(() => buildTimeline(score));
  const [page, pageMs] = time(() => renderTrack(score, 0, { width: 1280, scale: 1.2 }));
  const [line, lineMs] = time(() => renderTrack(score, 0, { width: 1280, mode: 'horizontal' }));
  const elements = (page.svg.match(/</g) || []).length;
  console.log(`  ${bytes.length} bytes, ${tl.notes.length} notes: parse ${parseMs.toFixed(1)} ms, timeline ${timelineMs.toFixed(1)} ms, render page ${pageMs.toFixed(1)} ms / line ${lineMs.toFixed(1)} ms, ${elements} SVG elements, ${(page.svg.length / 1024).toFixed(0)} KB`);
  assert.ok(parseMs < 500);
  assert.ok(timelineMs < 300);
  assert.ok(pageMs < 1500 && lineMs < 1500);
  assert.ok(tl.notes.length > 10000);
  assert.ok(line.width > 50000);
});

test('typical lesson renders in a few milliseconds', async () => {
  const score = await loadScore(writeGp(hugeSong(4), '5.10'));
  const runs = 50;
  const [, ms] = time(() => { for (let i = 0; i < runs; i++) renderTrack(score, 0, { width: 960 }); });
  console.log(`  4-bar exercise: ${(ms / runs).toFixed(2)} ms per render`);
  assert.ok(ms / runs < 20);
});
