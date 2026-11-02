// SPDX-License-Identifier: Apache-2.0
// Performance budgets on a deliberately huge score (400 bars of 16th-note chords), far bigger
// than a typical lesson, measured on the engine as the website runs it (WebAssembly). Budgets
// are generous so slow CI machines pass; the measured numbers are printed for comparison.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { core } from './core.mjs';
import { layoutSvg } from '../build/web/core.mjs';
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

test('huge score: open, layout and audio stay fast', () => {
  const bytes = writeGp(hugeSong(), '5.10');
  const [, openMs] = time(() => core.load(bytes)); // parse, timeline and the synth's note buffers
  const tl = core.timeline();
  const [page, pageMs] = time(() => core.layout({ width: 1280, scale: 1.2 }));
  const [line, lineMs] = time(() => core.layout({ width: 1280, horizontal: true }));
  const [svg, svgMs] = time(() => layoutSvg(page, 1.2));
  core.play(0, 1, true, true);
  const [, audioMs] = time(() => { for (let i = 0; i < 100; i++) core.render(2048); }); // 4.3 s of audio at 48 kHz
  console.log(`  ${bytes.length} bytes, ${tl.notes.length} notes: open ${openMs.toFixed(1)} ms, layout page ${pageMs.toFixed(1)} ms / line ${lineMs.toFixed(1)} ms, SVG ${svgMs.toFixed(1)} ms (${(svg.length / 1024).toFixed(0)} KB), audio ${(audioMs / 4.27).toFixed(1)} ms per second of sound`);
  assert.ok(openMs < 1500);
  assert.ok(pageMs < 1500 && lineMs < 1500 && svgMs < 1500);
  assert.ok(audioMs < 1000, 'audio renders much faster than real time');
  assert.ok(tl.notes.length > 10000);
  assert.ok(line.width > 50000);
});

test('typical lesson lays out in a few milliseconds', () => {
  core.load(writeGp(hugeSong(4), '5.10'));
  const runs = 50;
  const [, ms] = time(() => { for (let i = 0; i < runs; i++) layoutSvg(core.layout({ width: 960 }), 1); });
  console.log(`  4-bar exercise: ${(ms / runs).toFixed(2)} ms per layout and SVG`);
  assert.ok(ms / runs < 20);
});
