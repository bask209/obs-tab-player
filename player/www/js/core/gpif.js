// SPDX-License-Identifier: Apache-2.0
// Reader for GPIF, the XML score inside Guitar Pro 6 (.gpx) and Guitar Pro 7/8 (.gp) files.

import { parseXml, child, children, text, path } from './xml.js';
import { newScore, newBeat, newNote, finishScore } from './model.js';

const NOTE_VALUES = { Whole: 1, Half: 2, Quarter: 4, Eighth: 8, '16th': 16, '32nd': 32, '64th': 64, '128th': 128, DoubleWhole: 1 };
const DYNAMICS = { PPP: 1, PP: 2, P: 3, MP: 4, MF: 5, F: 6, FF: 7, FFF: 8 };
const TEMPO_UNIT = { 1: 0.5, 2: 1, 3: 1.5, 4: 2, 5: 3 }; // reference note -> quarters

const ids = (s) => (s || '').trim().split(/\s+/).filter(Boolean).map(Number);

function props(node) {
  const out = {};
  for (const p of children(child(node, 'Properties'), 'Property')) out[p.attrs.name] = p;
  return out;
}

export function readGpif(xmlText, format) {
  const doc = parseXml(xmlText);
  const gpif = child(doc, 'GPIF');
  if (!gpif) throw new Error('Not a GPIF document');
  const score = newScore(format);

  const s = child(gpif, 'Score');
  score.title = text(s, 'Title');
  score.subTitle = text(s, 'SubTitle');
  score.artist = text(s, 'Artist');
  score.album = text(s, 'Album');
  score.words = text(s, 'Words');
  score.music = text(s, 'Music');
  score.copyright = text(s, 'Copyright');
  score.tab = text(s, 'Tabber');
  score.instructions = text(s, 'Instructions');
  score.notices = text(s, 'Notices');

  // Index the flat element pools by id.
  const pool = (listName, itemName) => {
    const m = new Map();
    for (const n of children(child(gpif, listName), itemName)) m.set(n.attrs.id, n);
    return m;
  };
  const barsById = pool('Bars', 'Bar');
  const voicesById = pool('Voices', 'Voice');
  const beatsById = pool('Beats', 'Beat');
  const notesById = pool('Notes', 'Note');
  const rhythmsById = pool('Rhythms', 'Rhythm');

  // Tempo automations
  const tempos = [];
  for (const a of children(path(gpif, 'MasterTrack', 'Automations'), 'Automation')) {
    if (text(a, 'Type') !== 'Tempo') continue;
    const parts = text(a, 'Value').split(/\s+/).map(Number);
    const bpm = parts[0] * (TEMPO_UNIT[parts[1]] ?? 1);
    tempos.push({ bar: Number(text(a, 'Bar')) || 0, ratio: Number(text(a, 'Position')) || 0, bpm });
  }
  tempos.sort((a, b) => a.bar - b.bar || a.ratio - b.ratio);
  const first = tempos.find((t) => t.bar === 0 && t.ratio === 0) || tempos[0];
  score.tempo = first ? Math.round(first.bpm) : 120;

  // Tracks
  const staffCounts = [];
  for (const t of children(child(gpif, 'Tracks'), 'Track')) {
    const track = { name: text(t, 'Name'), tuning: [], capo: 0, percussion: false, bars: [], chords: new Map() };
    const staves = children(child(t, 'Staves'), 'Staff');
    staffCounts.push(Math.max(1, staves.length));
    const p = { ...props(t), ...(staves[0] ? props(staves[0]) : {}) };
    const pitches = ids(text(p.Tuning, 'Pitches'));
    track.tuning = pitches.reverse(); // GPIF lists low to high
    track.capo = Number(text(p.CapoFret, 'Fret')) || 0;
    if (!track.tuning.length) track.percussion = true;
    for (const name of ['DiagramCollection', 'ChordCollection']) {
      for (const item of children(child(p[name], 'Items'), 'Item')) track.chords.set(item.attrs.id, item.attrs.name);
    }
    const iset = text(path(t, 'InstrumentSet'), 'Type');
    if (/drum|percussion/i.test(iset)) track.percussion = true;
    score.tracks.push(track);
  }

  // Master bars
  let prev = { num: 4, den: 4 };
  const masterNodes = children(child(gpif, 'MasterBars'), 'MasterBar');
  masterNodes.forEach((mbNode, index) => {
    const [num, den] = (text(mbNode, 'Time') || `${prev.num}/${prev.den}`).split('/').map(Number);
    const rep = child(mbNode, 'Repeat');
    const mb = {
      num: num || 4, den: den || 4,
      repeatStart: rep?.attrs.start === 'true',
      repeatPlays: rep?.attrs.end === 'true' ? Math.max(2, Number(rep.attrs.count) || 2) : 0,
      alternate: ids(text(mbNode, 'AlternateEndings')).reduce((m, n) => m | (1 << (n - 1)), 0),
      marker: text(path(mbNode, 'Section'), 'Text'),
      doubleBar: !!child(mbNode, 'DoubleBar'),
    };
    const changes = tempos.filter((t) => t.bar === index && (index > 0 || t.ratio > 0));
    if (changes.length) mb.tempoChanges = changes.map((t) => ({ ratio: t.ratio, bpm: t.bpm }));
    score.masterBars.push(mb);
    prev = mb;

    const barIds = ids(text(mbNode, 'Bars'));
    let k = 0;
    score.tracks.forEach((track, ti) => {
      const barNode = barsById.get(String(barIds[k]));
      k += staffCounts[ti];
      track.bars.push(readBar(barNode, track));
    });
  });

  function readBar(barNode, track) {
    const bar = { voices: [] };
    if (!barNode) return bar;
    for (const vid of ids(text(barNode, 'Voices'))) {
      if (vid < 0) continue;
      const voiceNode = voicesById.get(String(vid));
      if (!voiceNode) continue;
      const beats = [];
      for (const bid of ids(text(voiceNode, 'Beats'))) {
        const b = beatsById.get(String(bid));
        if (b) beats.push(readBeat(b, track));
      }
      bar.voices.push(beats);
    }
    return bar;
  }

  function readBeat(node, track) {
    const beat = newBeat();
    const rhythm = rhythmsById.get(child(node, 'Rhythm')?.attrs.ref);
    if (rhythm) {
      beat.value = NOTE_VALUES[text(rhythm, 'NoteValue')] || 4;
      beat.dots = Number(child(rhythm, 'AugmentationDot')?.attrs.count) || 0;
      const tup = child(rhythm, 'PrimaryTuplet');
      if (tup) beat.tuplet = [Number(tup.attrs.num), Number(tup.attrs.den)];
    }
    const dyn = DYNAMICS[text(node, 'Dynamic')];
    const chordId = text(node, 'Chord');
    if (chordId && track.chords.has(chordId)) beat.chord = track.chords.get(chordId);
    const free = text(node, 'FreeText');
    if (free) beat.text = free;
    if (child(node, 'GraceNotes')) beat.grace = true;
    const tremolo = text(node, 'Tremolo');
    if (tremolo) beat.tremolo = Number(tremolo.split('/')[1]) || 16;
    if (text(node, 'Fadding') === 'FadeIn') beat.fadeIn = true;
    const bp = props(node);
    const pick = text(bp.PickStroke, 'Direction');
    if (pick) beat.pickStroke = pick.toLowerCase();
    const brush = text(bp.Brush, 'Direction');
    if (brush) beat.brush = brush.toLowerCase();
    if (bp.Slapped) beat.slap = true;
    if (bp.Popped) beat.pop = true;

    const n = track.tuning.length;
    for (const nid of ids(text(node, 'Notes'))) {
      const nn = notesById.get(String(nid));
      if (!nn) continue;
      const p = props(nn);
      const s = Number(text(p.String, 'String'));
      if (!n || Number.isNaN(s)) continue; // percussion notes are not shown as tab
      const note = newNote(n - 1 - s, Number(text(p.Fret, 'Fret')) || 0);
      if (dyn) note.dynamic = dyn;
      const tie = child(nn, 'Tie');
      if (tie?.attrs.destination === 'true') note.tie = true;
      if (child(nn, 'LetRing')) note.letRing = true;
      if (child(nn, 'Vibrato')) note.vibrato = true;
      if (child(nn, 'AntiAccent')) note.ghost = true;
      const accent = Number(text(nn, 'Accent'));
      if (accent & 1) note.staccato = true;
      if (accent & 4) note.accent = 'heavy';
      else if (accent & 8) note.accent = true;
      if (p.PalmMuted) note.palmMute = true;
      if (p.Muted) note.dead = true;
      if (p.HopoOrigin) note.hammer = true;
      const slide = Number(text(p.Slide, 'Flags')) || 0;
      if (slide & 1) note.slide = 'shift';
      else if (slide & 2) note.slide = 'legato';
      else if (slide & 4) note.slide = 'outDown';
      else if (slide & 8) note.slide = 'outUp';
      if (slide & 16) note.slideIn = 'below';
      else if (slide & 32) note.slideIn = 'above';
      const ht = text(p.HarmonicType, 'HType');
      if (ht && ht !== 'NoHarmonic') {
        note.harmonic = ht.toLowerCase();
        const hf = Number(text(p.HarmonicFret, 'HFret'));
        if (hf) note.harmonicFret = hf;
      }
      if (p.Bended) {
        const f = (name) => Number(text(p[name], 'Float'));
        const pts = [{ t: 0, semis: (f('BendOriginValue') || 0) / 50 }];
        if (p.BendMiddleValue) {
          const mv = f('BendMiddleValue') / 50;
          pts.push({ t: (f('BendMiddleOffset1') || 50) / 100, semis: mv });
          if (p.BendMiddleOffset2) pts.push({ t: f('BendMiddleOffset2') / 100, semis: mv });
        }
        pts.push({ t: p.BendDestinationOffset ? f('BendDestinationOffset') / 100 : 1, semis: (f('BendDestinationValue') || 0) / 50 });
        note.bend = pts;
      }
      beat.notes.push(note);
    }
    beat.rest = beat.notes.length === 0;
    return beat;
  }

  for (const t of score.tracks) delete t.chords;
  return finishScore(score);
}
