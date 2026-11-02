// SPDX-License-Identifier: Apache-2.0
// Every format the engine reads, round-tripped through test files written by tools/gp-writer.mjs
// (Guitar Pro 3-5) and tests/helpers.mjs (Guitar Pro 6-8), read by the engine itself (core.mjs).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { loadScore } from './core.mjs';
import { writeGp } from '../tools/gp-writer.mjs';
import { DEMO_SONGS } from '../tools/demo-songs.mjs';
import { gpifFor, makeZip, diffSong, loopingGpx } from './helpers.mjs';
import { buildBcfs, compressBcfz } from './gpx-writer.mjs';

for (const version of ['3.00', '4.06', '5.00', '5.10']) {
  test(`Guitar Pro ${version} round trip of all demo songs`, async () => {
    for (const song of DEMO_SONGS) {
      const score = await loadScore(writeGp(song, version));
      assert.equal(score.format, version.startsWith('5') ? 'gp5' : version.startsWith('4') ? 'gp4' : 'gp3');
      const skip = version === '3.00' ? ['pickStroke', 'palmMute', 'vibrato'] : [];
      assert.deepEqual(diffSong(song, score, skip), [], `${song.title} v${version}`);
    }
  });
}

test('Guitar Pro 7/8 (.gp zip, deflate and stored) round trip', async () => {
  for (const song of DEMO_SONGS) {
    const xml = Buffer.from(gpifFor(song));
    for (const deflate of [true, false]) {
      const zip = makeZip([['VERSION', Buffer.from('7.0')], ['Content/score.gpif', xml]], deflate);
      const score = await loadScore(zip);
      assert.equal(score.format, 'gp7');
      assert.deepEqual(diffSong(song, score), [], song.title);
    }
  }
});

test('Guitar Pro 6 (.gpx BCFS and BCFZ) round trip', async () => {
  for (const song of DEMO_SONGS) {
    const fsys = buildBcfs([['misc.xml', new TextEncoder().encode('<x/>')], ['score.gpif', new TextEncoder().encode(gpifFor(song))]]);
    for (const bytes of [fsys, compressBcfz(fsys)]) {
      const score = await loadScore(bytes);
      assert.equal(score.format, 'gpx');
      assert.deepEqual(diffSong(song, score), [], song.title);
    }
  }
});

test('score notes are kept verbatim', async () => {
  const s = await loadScore(writeGp(DEMO_SONGS[0], '5.10'));
  assert.match(s.notices, /Description: A minor pentatonic/);
  assert.match(s.notices, /Tip: /);
});

test('Guitar Pro 4 and 5 keep every technique', async () => {
  const song = DEMO_SONGS.find((s) => s.title.includes('Techniques'));
  for (const version of ['4.06', '5.10']) {
    const s = await loadScore(writeGp(song, version));
    const [b1, b2, b3, b4] = s.tracks[0].bars.map((b) => b.voices[0]);
    const v = `v${version}`;
    assert.ok(b1[0].notes.every((n) => n.harmonic === 'natural' && n.letRing), `${v} harmonics`);
    assert.ok(b1[2].notes.every((n) => n.dead), `${v} dead notes`);
    assert.equal(b1[4].notes[0].accent, version === '5.10' ? 'heavy' : true, `${v} accent`);
    assert.deepEqual({ ...b2[0].notes[0].grace, dynamic: undefined }, { fret: 12, hammer: true, dynamic: undefined }, `${v} grace`);
    assert.deepEqual(b2[2].notes[0].bend.map((p) => p.semis), [2, 0, 0], `${v} release`);
    assert.ok(b2[3].vibrato, `${v} vibrato`);
    assert.equal(b3[0].notes[0].slide, 'shift', `${v} slide`);
    assert.deepEqual([b3[2].notes[0].trill, b3[2].notes[0].trillSpeed], [16, 16], `${v} trill`);
    assert.equal(b3[3].tremolo, 16, `${v} tremolo`);
    assert.ok(b4[0].tap, `${v} tap`);
    assert.deepEqual(b4[3].whammy.map((p) => p.semis), [0, -2, 0], `${v} whammy`);
  }
  // Guitar Pro 5 also stores the musical directions
  const marked = { ...song, masterBars: song.masterBars.map((mb, i) => ({ ...mb, ...[{ targets: 4 }, { jump: 13 }, { jump: 6 }, { targets: 1 }][i] })) };
  const s = await loadScore(writeGp(marked, '5.10'));
  assert.deepEqual(s.masterBars.map((m) => [m.targets || 0, m.jump || 0]), [[4, 0], [0, 13], [0, 6], [1, 0]]);
});

test('demo files in docs/demo match the generator', async () => {
  const dir = path.join(import.meta.dirname, '..', 'docs', 'demo');
  for (const song of DEMO_SONGS) {
    const score = await loadScore(fs.readFileSync(path.join(dir, `${song.title}.gp5`)));
    assert.deepEqual(diffSong(song, score), []);
  }
});

test('rejects unknown and truncated files cleanly', async () => {
  assert.throws(() => loadScore(new TextEncoder().encode('hello world, not a tab')), /Unsupported/);
  const good = writeGp(DEMO_SONGS[1], '5.10');
  for (const cut of [40, 200, 900, good.length - 20]) {
    assert.throws(() => loadScore(good.subarray(0, cut)), Error);
  }
  // a GPX sector pointer that points backwards (found by the fuzzer: it used to loop forever)
  assert.throws(() => loadScore(loopingGpx()), Error);
});

test('Guitar Pro 7 XML: CDATA, entities, comments and self-closing tags', () => {
  const xml = gpifFor({ ...DEMO_SONGS[0], title: '<raw> & text' })
    .replace('<Name><![CDATA[Guitar]]></Name>', '<!-- note --><Name>Lead &amp; rhythm &lt;1&gt;&#33;</Name>');
  const score = loadScore(makeZip([['Content/score.gpif', Buffer.from(xml)]]));
  assert.equal(score.title, '<raw> & text');
  assert.equal(score.tracks[0].name, 'Lead & rhythm <1>!');
});

test('optional: every file in TAB_LIBRARY parses', { skip: !process.env.TAB_LIBRARY }, async () => {
  const root = process.env.TAB_LIBRARY;
  const files = [];
  const walk = (d) => { for (const e of fs.readdirSync(d, { withFileTypes: true })) { const p = path.join(d, e.name); if (e.isDirectory()) walk(p); else if (/\.(gp[345x]?|gp[78])$/i.test(e.name)) files.push(p); } };
  walk(root);
  const failed = [];
  for (const f of files) { try { await loadScore(fs.readFileSync(f)); } catch (e) { failed.push(`${path.basename(f)}: ${e.message}`); } }
  assert.deepEqual(failed, []);
  console.log(`  parsed ${files.length} files from TAB_LIBRARY`);
});
