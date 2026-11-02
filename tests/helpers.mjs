// SPDX-License-Identifier: Apache-2.0
// Test helpers: build GPIF documents (Guitar Pro 6/7/8 XML) and zip files from the
// simple song format used by tools/gp-writer.mjs.

import zlib from 'node:zlib';
import { buildBcfs } from './gpx-writer.mjs';

const NOTE_VALUE = { 1: 'Whole', 2: 'Half', 4: 'Quarter', 8: 'Eighth', 16: '16th', 32: '32nd', 64: '64th' };
const xmlEsc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);

export function gpifFor(song) {
  const bars = []; const voices = []; const beats = []; const notes = []; const rhythms = new Map();
  const rhythmId = (b) => {
    const key = `${b.value}|${b.dots || 0}|${b.tuplet ? b.tuplet.join('/') : ''}`;
    if (!rhythms.has(key)) rhythms.set(key, { id: rhythms.size, b });
    return rhythms.get(key).id;
  };
  const chordIds = new Map();
  const track = song.tracks[0];
  const masters = song.masterBars.map((mb, i) => {
    const bar = track.bars[i];
    const voiceIds = bar.voices.map((v) => {
      const beatIds = v.map((b) => {
        const noteIds = (b.rest ? [] : b.notes).map((n) => {
          const p = [`<Property name="String"><String>${track.tuning.length - 1 - n.string}</String></Property>`, `<Property name="Fret"><Fret>${n.fret}</Fret></Property>`];
          if (n.palmMute) p.push('<Property name="PalmMuted"><Enabled/></Property>');
          if (n.dead) p.push('<Property name="Muted"><Enabled/></Property>');
          if (n.hammer) p.push('<Property name="HopoOrigin"><Enabled/></Property>');
          if (n.slide) p.push(`<Property name="Slide"><Flags>${n.slide === 'legato' ? 2 : 1}</Flags></Property>`);
          if (n.harmonic) p.push(`<Property name="HarmonicType"><HType>${n.harmonic[0].toUpperCase() + n.harmonic.slice(1)}</HType></Property><Property name="HarmonicFret"><HFret>${n.harmonicFret || 12}</HFret></Property>`);
          if (n.bend) {
            const peak = Math.max(...n.bend.map((x) => x.semis));
            p.push('<Property name="Bended"><Enabled/></Property>');
            p.push(`<Property name="BendOriginValue"><Float>${n.bend[0].semis * 50}</Float></Property>`);
            p.push(`<Property name="BendDestinationValue"><Float>${n.bend[n.bend.length - 1].semis * 50}</Float></Property>`);
            if (n.bend.length > 2) p.push(`<Property name="BendMiddleValue"><Float>${peak * 50}</Float></Property>`);
          }
          const extra = `${n.letRing ? '<LetRing/>' : ''}${n.vibrato ? '<Vibrato>Slight</Vibrato>' : ''}${n.tie ? '<Tie origin="false" destination="true"/>' : ''}${n.accent ? '<Accent>8</Accent>' : ''}${n.trill != null ? `<Trill>${n.trill}</Trill>` : ''}`;
          notes.push(`<Note id="${notes.length}">${extra}<Properties>${p.join('')}</Properties></Note>`);
          return notes.length - 1;
        });
        let chord = '';
        if (b.chord) {
          if (!chordIds.has(b.chord)) chordIds.set(b.chord, chordIds.size);
          chord = `<Chord>${chordIds.get(b.chord)}</Chord>`;
        }
        const bp = [];
        if (b.pickStroke) bp.push(`<Property name="PickStroke"><Direction>${b.pickStroke === 'up' ? 'Up' : 'Down'}</Direction></Property>`);
        if (b.whammy) {
          bp.push('<Property name="WhammyBar"><Enable/></Property>');
          const w = b.whammy; // [origin, middle, destination] in semitones, or points { t, semis }
          const [o, m, d] = typeof w[0] === 'number' ? w : [w[0].semis, w.length > 2 ? w[1].semis : null, w[w.length - 1].semis];
          bp.push(`<Property name="WhammyBarOriginValue"><Float>${o * 50}</Float></Property><Property name="WhammyBarDestinationValue"><Float>${d * 50}</Float></Property>`);
          if (m != null) bp.push(`<Property name="WhammyBarMiddleValue"><Float>${m * 50}</Float></Property>`);
        }
        const props = bp.length ? `<Properties>${bp.join('')}</Properties>` : '';
        const extra = `${b.grace ? `<GraceNotes>${b.grace}</GraceNotes>` : ''}${b.dynamic ? `<Dynamic>${b.dynamic}</Dynamic>` : ''}`;
        beats.push(`<Beat id="${beats.length}">${extra}<Rhythm ref="${rhythmId(b)}"/>${chord}${noteIds.length ? `<Notes>${noteIds.join(' ')}</Notes>` : ''}${props}</Beat>`);
        return beats.length - 1;
      });
      voices.push(`<Voice id="${voices.length}"><Beats>${beatIds.join(' ')}</Beats></Voice>`);
      return voices.length - 1;
    });
    while (voiceIds.length < 4) voiceIds.push(-1);
    bars.push(`<Bar id="${bars.length}"><Clef>G2</Clef><Voices>${voiceIds.join(' ')}</Voices></Bar>`);
    const rep = mb.repeatStart || mb.repeatPlays ? `<Repeat start="${!!mb.repeatStart}" end="${!!mb.repeatPlays}" count="${mb.repeatPlays || 0}"/>` : '';
    const alt = mb.alternate ? `<AlternateEndings>${mb.alternate}</AlternateEndings>` : '';
    const dirs = mb.targets || mb.jump ? `<Directions>${(mb.targets || []).map((t) => `<Target>${t}</Target>`).join('')}${mb.jump ? `<Jump>${mb.jump}</Jump>` : ''}</Directions>` : '';
    const ferm = mb.fermatas ? `<Fermatas>${mb.fermatas.map((f) => `<Fermata><Type>Medium</Type><Length>${f.length}</Length><Offset>${f.offset}</Offset></Fermata>`).join('')}</Fermatas>` : '';
    const sec = mb.marker ? `<Section><Letter><![CDATA[A]]></Letter><Text><![CDATA[${mb.marker}]]></Text></Section>` : '';
    return `<MasterBar><Key><AccidentalCount>0</AccidentalCount><Mode>Major</Mode></Key><Time>${mb.num}/${mb.den}</Time>${rep}${alt}${sec}${dirs}${ferm}<Bars>${bars.length - 1}</Bars></MasterBar>`;
  });
  const chordItems = [...chordIds].map(([name, id]) => `<Item id="${id}" name="${xmlEsc(name)}"/>`).join('');
  const rhythmXml = [...rhythms.values()].map(({ id, b }) => `<Rhythm id="${id}"><NoteValue>${NOTE_VALUE[b.value]}</NoteValue>${b.dots ? `<AugmentationDot count="${b.dots}"/>` : ''}${b.tuplet ? `<PrimaryTuplet num="${b.tuplet[0]}" den="${b.tuplet[1]}"/>` : ''}</Rhythm>`).join('');
  return `<?xml version="1.0" encoding="utf-8"?>
<GPIF version="7">
<Score><Title><![CDATA[${song.title}]]></Title><SubTitle><![CDATA[]]></SubTitle><Artist><![CDATA[${song.artist}]]></Artist><Notices><![CDATA[${(song.notices || []).join('\n')}]]></Notices></Score>
<MasterTrack><Tracks>0</Tracks><Automations><Automation><Type>Tempo</Type><Linear>false</Linear><Bar>0</Bar><Position>0</Position><Visible>true</Visible><Value>${song.tempo} 2</Value></Automation></Automations></MasterTrack>
<Tracks><Track id="0"><Name><![CDATA[${track.name}]]></Name><Staves><Staff><Properties><Property name="Tuning"><Pitches>${[...track.tuning].reverse().join(' ')}</Pitches></Property><Property name="CapoFret"><Fret>0</Fret></Property><Property name="DiagramCollection"><Items>${chordItems}</Items></Property></Properties></Staff></Staves></Track></Tracks>
<MasterBars>${masters.join('')}</MasterBars>
<Bars>${bars.join('')}</Bars><Voices>${voices.join('')}</Voices><Beats>${beats.join('')}</Beats><Notes>${notes.join('')}</Notes><Rhythms>${rhythmXml}</Rhythms>
</GPIF>`;
}

const CRC = new Int32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c; });
const crc32 = (b) => { let c = -1; for (const x of b) c = CRC[(c ^ x) & 0xff] ^ (c >>> 8); return (c ^ -1) >>> 0; };

export function makeZip(entries, deflate = true) {
  const locals = []; const centrals = []; let offset = 0;
  for (const [name, data] of entries) {
    const nameB = Buffer.from(name);
    const body = deflate ? zlib.deflateRawSync(data) : Buffer.from(data);
    const h = Buffer.alloc(30);
    h.writeUInt32LE(0x04034b50, 0); h.writeUInt16LE(20, 4); h.writeUInt16LE(deflate ? 8 : 0, 8);
    h.writeUInt32LE(crc32(data), 14); h.writeUInt32LE(body.length, 18); h.writeUInt32LE(data.length, 22); h.writeUInt16LE(nameB.length, 26);
    const c = Buffer.alloc(46);
    c.writeUInt32LE(0x02014b50, 0); c.writeUInt16LE(20, 4); c.writeUInt16LE(20, 6); c.writeUInt16LE(deflate ? 8 : 0, 10);
    c.writeUInt32LE(crc32(data), 16); c.writeUInt32LE(body.length, 20); c.writeUInt32LE(data.length, 24); c.writeUInt16LE(nameB.length, 28); c.writeUInt32LE(offset, 42);
    locals.push(h, nameB, body);
    centrals.push(c, nameB);
    offset += 30 + nameB.length + body.length;
  }
  const cd = Buffer.concat(centrals);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(entries.length, 8); end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(cd.length, 12); end.writeUInt32LE(offset, 16);
  return new Uint8Array(Buffer.concat([...locals, cd, end]));
}

// Compare a parsed score with the source song (notes, rhythm, structure).
// `skip` lists features a format cannot store (e.g. GP3 has no pick strokes or palm mutes).
export function diffSong(song, score, skip = []) {
  const errs = [];
  if (score.title !== song.title) errs.push(`title ${score.title}`);
  if (score.tempo !== song.tempo) errs.push(`tempo ${score.tempo}`);
  if (score.masterBars.length !== song.masterBars.length) errs.push('bar count');
  song.masterBars.forEach((mb, i) => {
    const m = score.masterBars[i];
    if (!m) return;
    if (m.num !== mb.num || m.den !== mb.den) errs.push(`meter ${i}`);
    if (m.repeatStart !== !!mb.repeatStart || m.repeatPlays !== (mb.repeatPlays || 0)) errs.push(`repeat ${i}`);
    if (m.alternate !== (mb.alternate ? 1 << (mb.alternate - 1) : 0)) errs.push(`alternate ${i}`);
    if (m.marker !== (mb.marker || '')) errs.push(`marker ${i}`);
    const src = song.tracks[0].bars[i].voices[0];
    const got = score.tracks[0].bars[i].voices[0];
    if (src.length !== got.length) { errs.push(`beats ${i}`); return; }
    src.forEach((b, k) => {
      const g = got[k];
      if (g.value !== b.value || g.dots !== (b.dots || 0)) errs.push(`duration ${i}.${k}`);
      if (!!g.tuplet !== !!b.tuplet) errs.push(`tuplet ${i}.${k}`);
      if ((g.chord || '') !== (b.chord || '')) errs.push(`chord ${i}.${k}`);
      if (!skip.includes('pickStroke') && (g.pickStroke || '') !== (b.pickStroke || '')) errs.push(`pick ${i}.${k}`);
      const sn = (b.rest ? [] : b.notes).map((x) => `${x.string}:${x.fret}`).sort().join();
      const gn = g.notes.map((x) => `${x.string}:${x.fret}`).sort().join();
      if (sn !== gn) errs.push(`notes ${i}.${k} [${sn}] vs [${gn}]`);
      for (const f of ['hammer', 'palmMute', 'letRing', 'vibrato'].filter((x) => !skip.includes(x))) {
        if ((b.notes || []).some((x) => x[f]) !== g.notes.some((x) => x[f])) errs.push(`${f} ${i}.${k}`);
      }
      if ((b.notes || []).some((x) => x.bend) !== g.notes.some((x) => x.bend)) errs.push(`bend ${i}.${k}`);
    });
  });
  return errs;
}

// A short tab using the effects that change playback: grace notes, whammy bar, trill, tapped and
// natural harmonics, dynamics, a fermata and a D.S. al Coda. Guitar Pro 6-8 only (GPIF).
export const EFFECTS_SONG = (() => {
  const STANDARD = [64, 59, 55, 50, 45, 40];
  const mb = (o = {}) => ({ num: 4, den: 4, ...o });
  const q = (fret, o = {}, b = {}) => ({ value: 4, notes: [{ string: 2, fret, ...o }], ...b });
  return {
    title: 'Effects', artist: '', tempo: 90,
    masterBars: [mb({ targets: ['Segno'] }), mb({ jump: 'DaCoda', fermatas: [{ offset: '3/1', length: 0.5 }] }), mb({ jump: 'DaSegnoAlCoda' }), mb({ targets: ['Coda'] })],
    tracks: [{
      name: 'Guitar', tuning: STANDARD,
      bars: [
        { voices: [[q(5), { value: 32, grace: 'BeforeBeat', notes: [{ string: 2, fret: 5, hammer: true }] }, q(7), q(7, {}, { whammy: [0, -2, 0] }), q(5, { trill: 7 })]] },
        { voices: [[q(5, { harmonic: 'Tap', harmonicFret: 7 }), q(3, { harmonic: 'Natural', harmonicFret: 3.2 }), q(7, {}, { dynamic: 'P' }), q(9, {}, { dynamic: 'FF' })]] },
        { voices: [[{ value: 1, notes: [{ string: 2, fret: 12 }] }]] },
        { voices: [[{ value: 1, notes: [{ string: 1, fret: 12 }] }]] },
      ],
    }],
  };
})();

// A GPX container whose second file points back at an earlier sector: readers must not loop on it.
export function loopingGpx() {
  const pad = new Uint8Array(16).fill(1);
  const bcfs = buildBcfs([['misc.xml', pad], ['score.gpif', pad]]);
  // sectors: 1 entry misc, 2 its data, 3 entry score, 4 its data; point score's data at sector 1
  new DataView(bcfs.buffer).setInt32(4 + 3 * 0x1000 + 0x94, 1, true);
  return bcfs;
}
