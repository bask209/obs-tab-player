// SPDX-License-Identifier: Apache-2.0
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { loadScore } from '../player/www/js/core/load.js';
import { parseXml, text, child } from '../player/www/js/core/xml.js';
import { writeGp } from '../tools/gp-writer.mjs';
import { DEMO_SONGS } from '../tools/demo-songs.mjs';
import { gpifFor, makeZip, diffSong } from './helpers.mjs';
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

test('demo files in docs/demo match the generator', async () => {
  const dir = path.join(import.meta.dirname, '..', 'docs', 'demo');
  for (const song of DEMO_SONGS) {
    const score = await loadScore(fs.readFileSync(path.join(dir, `${song.title}.gp5`)));
    assert.deepEqual(diffSong(song, score), []);
  }
});

test('rejects unknown and truncated files cleanly', async () => {
  await assert.rejects(loadScore(new TextEncoder().encode('hello world, not a tab')), /Unsupported/);
  const good = writeGp(DEMO_SONGS[1], '5.10');
  for (const cut of [40, 200, 900, good.length - 20]) {
    await assert.rejects(loadScore(good.subarray(0, cut)), Error);
  }
});

test('XML reader handles CDATA, entities, attributes and self-closing tags', () => {
  const doc = parseXml('<?xml version="1.0"?><a x="1 &amp; 2"><b><![CDATA[<raw> & text]]></b><c/><d>&lt;ok&#33;</d><!-- note --></a>');
  const a = child(doc, 'a');
  assert.equal(a.attrs.x, '1 & 2');
  assert.equal(text(a, 'b'), '<raw> & text');
  assert.ok(child(a, 'c'));
  assert.equal(text(a, 'd'), '<ok!');
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
