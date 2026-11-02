// SPDX-License-Identifier: Apache-2.0
// Tab layout and SVG rendering for the browser edition and the website.
// layoutTrack() is a line-by-line port of plugin/src/core/layout.cpp and returns the same
// drawing primitives (tests/native-parity.test.mjs compares them), so the browser shows
// exactly what the OBS plugin draws. renderTrack() turns them into SVG whose colours come
// from CSS classes (see tab.css), so themes change without re-rendering.

import { QUARTER, barTicks, beatTicks } from './model.js';
import { barFeel } from './timeline.js';

export const Role = {
  Staff: 0, BarLine: 1, BarThick: 2, RepeatDot: 3, Note: 4, NoteSoft: 5, Clef: 6, StringName: 7, TimeSig: 8, BarNumber: 9, Marker: 10,
  Chord: 11, BeatText: 12, SmallText: 13, Technique: 14, TechniqueFill: 15, Dashed: 16, Stem: 17, Beam: 18, Rest: 19, Arrow: 20,
};
export const PrimType = { Line: 0, Rect: 1, Text: 2, Path: 3, Circle: 4 };

export const DEFAULT_OPTIONS = {
  width: 900,
  mode: 'page', // page | horizontal
  scale: 1,
  rhythm: true,
  barNumbers: true,
  chords: true,
  techniques: true,
  labels: 'tab', // tab | names | none
  spacing: 1, // horizontal density multiplier
};

const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const QUARTER_NOTE = '♩';
const utf8Length = (s) => new TextEncoder().encode(s).length; // the C++ layout measures text in bytes
const roundAway = (v) => Math.sign(v) * Math.round(Math.abs(v)); // std::round

// ------------------------------------------------------------------ labels

function fretText(f) {
  const tenths = Math.round(f * 10);
  return tenths % 10 ? `${Math.trunc(tenths / 10)}.${tenths % 10}` : String(Math.trunc(tenths / 10));
}

export function noteLabel(n) {
  if (n.dead) return 'x';
  if (n.harmonic === 'natural') return `<${fretText(n.harmonicFret > 0 ? n.harmonicFret : n.fret)}>`;
  if (n.harmonic === 'artificial' || n.harmonic === 'pinch') return String(n.fret);
  if (n.ghost || n.tie) return `(${n.fret})`;
  return String(n.fret);
}

// Small text written after the fret, as Guitar Pro does: the trill fret, or where a harmonic sounds.
function noteSuffix(n) {
  if (n.trill != null && n.trill >= 0) return `(${n.trill})`;
  if (n.harmonicFret > 0 && n.harmonic && n.harmonic !== 'natural') return `<${fretText(n.fret + n.harmonicFret)}>`;
  return '';
}

function bendLabel(semis) {
  const steps = roundAway(semis * 2) / 4;
  if (steps <= 0) return '';
  if (steps === 0.25) return '¼';
  if (steps === 0.5) return '½';
  if (steps === 1) return 'full';
  const whole = Math.floor(steps);
  const frac = steps - whole;
  return `${whole}${frac === 0.5 ? '½' : frac === 0.25 ? '¼' : frac === 0.75 ? '¾' : ''}`;
}

// Whammy amounts in tones like Guitar Pro: -1, -½, +1½ ...
function whammyLabel(semis) {
  let m = bendLabel(Math.abs(semis));
  if (m === 'full') m = '1';
  return m ? (semis < 0 ? '-' : '+') + m : '0';
}

const DYNAMICS = ['', 'ppp', 'pp', 'p', 'mp', 'mf', 'f', 'ff', 'fff'];
const JUMP_NAMES = ['', 'D.C.', 'D.C. al Coda', 'D.C. al Double Coda', 'D.C. al Fine', 'D.S.', 'D.S. al Coda', 'D.S. al Double Coda', 'D.S. al Fine',
  'D.S.S.', 'D.S.S. al Coda', 'D.S.S. al Double Coda', 'D.S.S. al Fine', 'To Coda', 'To Double Coda'];
const HARMONIC_NAMES = { natural: 'N.H.', artificial: 'A.H.', tap: 'T.H.', pinch: 'P.H.', semi: 'S.H.' };
const SEGNO = '\u{1D10B}';
const CODA = '\u{1D10C}';
const SIGNS = [CODA, CODA + CODA, SEGNO, SEGNO + SEGNO, 'Fine'];

// Bend points reduced to the turns Guitar Pro draws: rises, holds and releases.
function bendShape(input) {
  const pts = [];
  for (const q of input) {
    const p = { t: Math.min(1, Math.max(0, q.t)), semis: roundAway(q.semis * 4) / 4 };
    if (pts.length && p.t < pts[pts.length - 1].t) continue;
    if (pts.length >= 2) {
      const d1 = pts[pts.length - 1].semis - pts[pts.length - 2].semis;
      const d2 = p.semis - pts[pts.length - 1].semis;
      if ((d1 > 0 && d2 > 0) || (d1 < 0 && d2 < 0) || (d1 === 0 && d2 === 0)) { pts[pts.length - 1] = p; continue; }
    }
    if (pts.length && p.semis === pts[pts.length - 1].semis && p.t === pts[pts.length - 1].t) continue;
    pts.push(p);
  }
  return pts;
}

// JS model fields as the C++ model has them
const accentOf = (n) => (n.accent === 'heavy' ? 2 : n.accent ? 1 : 0);
const trillOf = (n) => (n.trill != null && n.trill >= 0 ? n.trill : -1);
const dynOf = (n) => (n.dynamic >= 1 ? n.dynamic : 0);
const techniqueOf = (b) => (b.tap ? 1 : b.slap ? 2 : b.pop ? 3 : 0);
const tupNum = (b) => (b.tuplet ? b.tuplet[0] || 0 : 0);
const tupDen = (b) => (b.tuplet ? b.tuplet[1] || 0 : 0);
const lowerBound = (keys, tick) => keys.find((k) => k >= tick);

const NO_PATH = Object.freeze([]);

// One drawing primitive, as in plugin/src/core/layout.hpp.
class Prim {
  constructor(type, role, x0, y0, x1, y1, stroke, string = -1) {
    this.type = type; this.role = role;
    this.x0 = x0; this.y0 = y0; this.x1 = x1; this.y1 = y1;
    this.stroke = stroke; this.size = 12; this.align = 1; this.bold = false; this.italic = false;
    this.string = string; this.beatKey = -1; this.text = ''; this.path = NO_PATH;
  }
}

const M = (x, y) => ['M', x, y];
const L = (x, y) => ['L', x, y];
const Q = (cx, cy, x, y) => ['Q', cx, cy, x, y];
const C = (x1, y1, x2, y2, x, y) => ['C', x1, y1, x2, y2, x, y];
const Z = () => ['Z'];

// ------------------------------------------------------------------ layout

export function layoutTrack(score, trackIndex = 0, options = {}) {
  const o = { ...DEFAULT_OPTIONS, ...options };
  const horizontal = o.mode === 'horizontal';
  const labels = o.labels === 'names' ? 1 : o.labels === 'none' ? 2 : 0;
  const out = { width: 0, height: 0, scale: o.scale, prims: [], bars: [], systems: [], beats: new Map() };
  const track = score.tracks[trackIndex];
  if (!track) return out;
  const S = o.scale;
  const strings = Math.max(1, track.tuning.length || 6);
  const gap = 13 * S; const font = 12.5 * S; const digitW = font * 0.6; const margin = 8 * S;
  const clefW = labels === 2 ? 6 * S : 26 * S;

  const add = (p) => { out.prims.push(p); return p; };
  const B = {
    line: (r, x0, y0, x1, y1, w = 1) => add(new Prim(PrimType.Line, r, x0, y0, x1, y1, w)),
    rect: (r, x, y, w, h, beatKey = -1) => { const p = add(new Prim(PrimType.Rect, r, x, y, x + w, y + h, 1)); p.beatKey = beatKey; return p; },
    circle: (r, cx, cy, radius) => add(new Prim(PrimType.Circle, r, cx, cy, radius, 0, 1)),
    text: (r, x, y, text, size, align = 1) => { const p = add(new Prim(PrimType.Text, r, x, y, 0, 0, 1)); p.text = text; p.size = size; p.align = align; return p; },
    path: (r, path, w = 1.2) => { const p = add(new Prim(PrimType.Path, r, 0, 0, 0, 0, w)); p.path = path; return p; },
  };

  // horizontal metrics per bar
  const bars = score.masterBars.map((mb, i) => {
    const bar = track.bars[i] || { voices: [] };
    const slots = new Map();
    for (const v of bar.voices) {
      for (const beat of v) {
        let digits = 1;
        let side = 0; // room for a grace note or brush arrow before, or a suffix after
        for (const n of beat.notes) {
          digits = Math.max(digits, noteLabel(n).length);
          if (n.grace) side = Math.max(side, (n.grace.dead ? 1 : String(n.grace.fret).length) * digitW * 0.75 + 6 * S);
          const suffix = noteSuffix(n);
          if (suffix) side = Math.max(side, suffix.length * digitW * 0.7 + 3 * S);
        }
        if (beat.brush && !beat.rest) side = Math.max(side, 8 * S);
        const minW = digits * digitW + 7 * S + 2 * side;
        const w = Math.max(minW, (16 + 26 * Math.sqrt(Math.min(beat.ticks, QUARTER * 4) / QUARTER)) * S * o.spacing);
        const cur = slots.get(beat.start);
        if (!cur) slots.set(beat.start, { tick: beat.start, w, min: minW });
        else if (w < cur.w) cur.w = Math.max(cur.min, w);
      }
    }
    const list = [...slots.values()].sort((a, b) => a.tick - b.tick);
    if (!list.length) list.push({ tick: 0, w: 40 * S, min: 10 * S });
    const prev = score.masterBars[i - 1];
    const showTime = !prev || prev.num !== mb.num || prev.den !== mb.den;
    const pre = 12 * S + (showTime ? 24 * S : 0) + (mb.repeatStart ? 14 * S : 0);
    const post = 6 * S + (mb.repeatPlays ? 10 * S : 0);
    const content = list.reduce((s, x) => s + x.w, 0);
    return { index: i, mb, bar, slots: list, showTime, pre, post, natural: pre + content + post, sys: -1, slotX: new Map(), keys: [] };
  });

  // rows
  const systems = [];
  if (horizontal) {
    systems.push({ bars: bars.slice(), stretch: 1, last: false });
  } else {
    const avail = o.width - margin * 2 - clefW;
    let row = [];
    let w = 0;
    for (const b of bars) {
      if (row.length && w + b.natural > avail) { systems.push({ bars: row, stretch: 1, last: false }); row = []; w = 0; }
      row.push(b);
      w += b.natural;
    }
    if (row.length) systems.push({ bars: row, stretch: 1, last: true });
    for (const sys of systems) {
      let nat = 0; let fixed = 0; let floor = 0;
      for (const b of sys.bars) {
        nat += b.natural;
        fixed += b.pre + b.post;
        for (const sl of b.slots) floor = Math.max(floor, sl.min / sl.w);
      }
      const fill = sys.last && nat < avail * 0.7 ? 1 : (avail - fixed) / Math.max(1, nat - fixed);
      sys.stretch = Math.max(floor, fill);
    }
  }

  out.bars = new Array(bars.length);
  let y = margin;
  let maxWidth = 0;
  const defaultDyn = score.format === 'gp7' || score.format === 'gpx' ? 5 : 6;
  let dynBefore = defaultDyn; // last dynamic written
  const lastBar = bars.length - 1;
  systems.forEach((sys, si) => {
    const any = (pred) => sys.bars.some((b) => b.bar.voices.some((v) => v.some(pred)));
    const noteAny = (pred) => any((b) => b.notes.some(pred));
    const hasChords = o.chords && any((b) => b.chord);
    let hasText = any((b) => b.text);
    for (const b of sys.bars) hasText = hasText || !!b.mb.marker || !!b.mb.alternate || b.mb.repeatPlays > 2;
    const hasBend = o.techniques && noteAny((n) => n.bend && n.bend.length);
    const hasWhammy = o.techniques && any((b) => b.whammy && b.whammy.length && !b.rest);
    // Effect lanes above the tab: each kind of effect keeps one lane, as Guitar Pro stacks them, but
    // kinds that never meet along the row share a lane, so one long line of tab stays low. The kinds
    // nearest the strings come first. Bar and beat positions are worked out here ahead of drawing.
    const K = { Accent: 0, Pick: 1, TSP: 2, Fade: 3, Trem: 4, Vib: 5, Trill: 6, Harm: 7, PM: 8, Ring: 9 };
    const reach = Array.from({ length: 10 }, () => []); // x ranges each kind covers
    if (o.techniques) {
      let px = margin + clefW;
      for (const b of sys.bars) {
        const barX = px; let cx = barX;
        if (b.mb.repeatStart) cx += 14 * S;
        if (b.showTime) cx += 24 * S;
        cx += 12 * S;
        for (const sl of b.slots) { b.slotX.set(sl.tick, cx + sl.min / 2); cx += sl.w * sys.stretch; }
        cx += b.post - (b.mb.repeatPlays ? 10 * S : 0);
        cx += b.mb.repeatPlays ? 10 * S : b.index === lastBar ? 1 * S : 0;
        px = cx;
        const voice = b.bar.voices[0] || [];
        voice.forEach((beat, k) => {
          if (beat.rest) return;
          const bx = b.slotX.has(beat.start) ? b.slotX.get(beat.start) : barX + 10 * S;
          let endX = cx - 4 * S;
          if (k + 1 < voice.length && b.slotX.has(voice[k + 1].start)) endX = b.slotX.get(voice[k + 1].start);
          const effEnd = Math.max(bx + 12 * S, endX - 4 * S);
          const has = (pred) => beat.notes.some(pred);
          const add = (kd, a, c) => reach[kd].push([a, c]);
          if (has((n) => accentOf(n) > 0 || n.staccato)) add(K.Accent, bx - 6 * S, bx + 10 * S);
          if (beat.pickStroke) add(K.Pick, bx - 6 * S, bx + 6 * S);
          if (techniqueOf(beat)) add(K.TSP, bx - 6 * S, bx + 6 * S);
          if (beat.fadeIn) add(K.Fade, bx - 3 * S, effEnd + 1 * S);
          if (beat.tremolo > 0) add(K.Trem, bx - 6 * S, bx + 6 * S);
          if (beat.vibrato || has((n) => n.vibrato || n.wideVibrato)) add(K.Vib, bx - 3 * S, effEnd + 1 * S);
          if (has((n) => trillOf(n) >= 0)) add(K.Trill, bx - 6 * S, effEnd + 1 * S);
          // spans reach their label at the start and the hook at the end
          if (has((n) => n.harmonic)) add(K.Harm, bx - 6 * S, Math.max(effEnd, bx + 18 * S) + 6 * S);
          if (has((n) => n.palmMute)) add(K.PM, bx - 6 * S, Math.max(effEnd, bx + 18 * S) + 6 * S);
          if (has((n) => n.letRing)) add(K.Ring, bx - 6 * S, Math.max(effEnd, bx + 40 * S) + 6 * S);
        });
      }
    }
    // pack: merge each kind's ranges, then put it in the first lane it does not meet
    const merged = (v) => {
      const m = [];
      for (const r of [...v].sort((a, c) => a[0] - c[0] || a[1] - c[1])) {
        if (m.length && r[0] <= m[m.length - 1][1]) m[m.length - 1][1] = Math.max(m[m.length - 1][1], r[1]);
        else m.push([r[0], r[1]]);
      }
      return m;
    };
    const lanes = [];
    const laneOf = reach.map((list) => {
      if (!list.length) return -1;
      const mine = merged(list);
      const meets = (laneList) => {
        let i = 0; let j = 0;
        while (i < laneList.length && j < mine.length) {
          if (laneList[i][0] < mine[j][1] - 0.5 && mine[j][0] < laneList[i][1] - 0.5) return true;
          if (laneList[i][1] < mine[j][1]) i++; else j++;
        }
        return false;
      };
      let l = 0;
      while (l < lanes.length && meets(lanes[l])) l++;
      if (l === lanes.length) lanes.push([]);
      lanes[l] = merged([...lanes[l], ...mine]);
      return l;
    });
    let hasDir = false; let hasFermata = false;
    for (const b of sys.bars) {
      hasDir = hasDir || !!b.mb.targets || !!b.mb.jump;
      hasFermata = hasFermata || !!(b.mb.fermatas && b.mb.fermatas.length);
    }
    // dynamics are written where they change (the default, mf in Guitar Pro 6-8 and f before, is not)
    let hasDyn = false;
    {
      let last = dynBefore;
      for (const b of sys.bars) {
        for (const beat of b.bar.voices[0] || []) {
          if (!beat.notes.length) continue;
          let d = 0; // the loudest note of the beat, as drawn below
          for (const n of beat.notes) d = Math.max(d, dynOf(n) || defaultDyn);
          hasDyn = hasDyn || d !== last;
          last = d;
        }
      }
    }
    // tempo and swing marks (Guitar Pro writes them above the first bar and where they change)
    const barTempo = (b) => {
      let t = 0;
      for (const tc of b.mb.tempoChanges || []) if (tc.ratio <= 0.001) t = tc.bpm;
      for (const v of b.bar.voices) for (const beat of v) if (beat.tempo > 0 && beat.start === 0) t = beat.tempo;
      return t;
    };
    // tempo changes inside a bar: [tick, bpm]
    const midTempos = (b) => {
      const res = [];
      const len = barTicks(b.mb);
      for (const tc of b.mb.tempoChanges || []) if (tc.ratio > 0.001) res.push([Math.trunc(tc.ratio * len), tc.bpm]);
      for (const beat of b.bar.voices[0] || []) if (beat.tempo > 0 && beat.start > 0) res.push([beat.start, beat.tempo]);
      return res;
    };
    const feelOf = (i) => (i >= 0 ? barFeel(score, score.masterBars[i]) : '');
    let hasTempo = false;
    for (const b of sys.bars) {
      if (b.index === 0 || barTempo(b) > 0 || feelOf(b.index - 1) !== feelOf(b.index) || midTempos(b).length) hasTempo = true;
    }
    let top = 4 * S;
    const rowDir = top; if (hasDir) top += 18 * S;
    const rowTempo = top; if (hasTempo) top += 17 * S;
    const rowChord = top; if (hasChords) top += 16 * S;
    const rowText = top; if (hasText) top += 16 * S;
    const lane = 13 * S; const lanesTop = top;
    top += lanes.length * lane;
    const rowOf = (kd) => (laneOf[kd] < 0 ? lanesTop : lanesTop + (lanes.length - 1 - laneOf[kd]) * lane);
    const rowRing = rowOf(K.Ring); const rowPM = rowOf(K.PM); const rowHarm = rowOf(K.Harm); const rowTrill = rowOf(K.Trill); const rowVib = rowOf(K.Vib);
    const rowTrem = rowOf(K.Trem); const rowFade = rowOf(K.Fade); const rowTSP = rowOf(K.TSP); const rowPick = rowOf(K.Pick); const rowAccent = rowOf(K.Accent);
    const rowWhammy = top; if (hasWhammy) top += 24 * S;
    const rowFermata = top; if (hasFermata) top += 14 * S;
    if (hasBend) top += 18 * S;
    if (o.barNumbers && !hasText && !hasChords) top += 6 * S;
    const staffTop = y + top + 6 * S; const staffH = gap * (strings - 1); const staffBottom = staffTop + staffH;
    const rhythmTop = staffBottom + 7 * S;
    const dynY = staffBottom + (o.rhythm ? 44 * S : 16 * S);
    const bottom = staffBottom + (o.rhythm ? 34 * S : 10 * S) + (hasDyn && o.techniques ? 16 * S : 0);
    const sy = (s) => staffTop + s * gap;
    const gaps = Array.from({ length: strings }, () => []);
    let x = margin;
    const staffStart = out.prims.length; // staff lines are added last but drawn first

    if (labels === 0 && strings >= 3) {
      const mid = (staffTop + staffBottom) / 2; const step = Math.min(gap * 1.15, staffH / 3);
      ['T', 'A', 'B'].forEach((ch, k) => { B.text(Role.Clef, x + 9 * S, mid + (k - 1) * step, ch, font * 0.95).bold = true; });
      for (let s = 0; s < strings; s++) gaps[s].push([x + 2 * S, x + 16 * S]);
    } else if (labels === 1) {
      for (let s = 0; s < strings; s++) {
        const t = s < track.tuning.length ? track.tuning[s] : 40;
        B.text(Role.StringName, x + 8 * S, sy(s), NOTE_NAMES[((t % 12) + 12) % 12], font * 0.8).bold = true;
      }
    }
    const lineStart = x + (labels === 1 ? 18 * S : 0);
    x += clefW;
    B.line(Role.BarLine, lineStart, staffTop, lineStart, staffBottom, 1.2);
    const sg = { y, top: staffTop, bottom: staffBottom, height: 0, x0: lineStart, x1: 0 };

    const repeatDots = (cx) => {
      const mid = (staffTop + staffBottom) / 2; const off = Math.max(gap * 0.5, staffH * 0.15);
      B.circle(Role.RepeatDot, cx, mid - off, 1.9 * S);
      B.circle(Role.RepeatDot, cx, mid + off, 1.9 * S);
    };

    for (const b of sys.bars) {
      const mb = b.mb;
      const barX = x; let cx = barX;
      if (mb.repeatStart) {
        B.rect(Role.BarThick, cx + 1 * S, staffTop, 3 * S, staffH);
        B.line(Role.BarLine, cx + 7 * S, staffTop, cx + 7 * S, staffBottom, 1.2);
        repeatDots(cx + 11 * S);
        cx += 14 * S;
      }
      if (b.showTime) {
        const tx = cx + 13 * S; const mid = (staffTop + staffBottom) / 2;
        const fs = Math.min(staffH * 0.42, 24 * S);
        B.text(Role.TimeSig, tx, mid - staffH * 0.22, String(mb.num), fs).bold = true;
        B.text(Role.TimeSig, tx, mid + staffH * 0.22, String(mb.den), fs).bold = true;
        for (let s = 0; s < strings; s++) gaps[s].push([tx - 8 * S, tx + 8 * S]);
        cx += 24 * S;
      }
      cx += 12 * S;
      for (const sl of b.slots) {
        b.slotX.set(sl.tick, cx + sl.min / 2);
        cx += sl.w * sys.stretch;
      }
      b.keys = [...b.slotX.keys()];
      cx += b.post - (mb.repeatPlays ? 10 * S : 0);
      if (mb.repeatPlays) {
        repeatDots(cx - 1 * S);
        cx += 4 * S;
        B.line(Role.BarLine, cx, staffTop, cx, staffBottom, 1.2);
        B.rect(Role.BarThick, cx + 3 * S, staffTop, 3 * S, staffH);
        cx += 6 * S;
        if (mb.repeatPlays > 2) B.text(Role.SmallText, cx - 4 * S, y + rowText + 10 * S, `×${mb.repeatPlays}`, font * 0.75, 2);
      } else if (b.index === lastBar) {
        B.line(Role.BarLine, cx - 5 * S, staffTop, cx - 5 * S, staffBottom, 1.2);
        B.rect(Role.BarThick, cx - 2 * S, staffTop, 3 * S, staffH);
        cx += 1 * S;
      } else {
        B.line(Role.BarLine, cx, staffTop, cx, staffBottom, 1.2);
        if (mb.doubleBar) B.line(Role.BarLine, cx - 3 * S, staffTop, cx - 3 * S, staffBottom, 1.2);
      }
      const barEnd = cx;
      if (o.barNumbers) B.text(Role.BarNumber, barX + 2 * S, staffTop - 5 * S, String(b.index + 1), font * 0.68, 0);
      if (mb.targets) {
        // signs at the start of the bar: segno, coda, fine
        const sign = SIGNS.filter((_, k) => mb.targets & (1 << k)).join(' ');
        B.text(Role.Marker, barX + 3 * S, y + rowDir + 9 * S, sign, font * (mb.targets === 16 ? 0.85 : 1.5), 0).bold = true;
      }
      if (mb.jump) {
        const t = B.text(Role.Marker, barEnd - 4 * S, y + rowDir + 9 * S, JUMP_NAMES[mb.jump] || '', font * 0.82, 2);
        t.bold = t.italic = true;
      }
      for (const f of mb.fermatas || []) {
        const at = lowerBound(b.keys, Math.trunc(f.ratio * barTicks(mb)));
        const fx = at !== undefined ? b.slotX.get(at) : barEnd - 8 * S; const fy = y + rowFermata + 10 * S;
        B.path(Role.Technique, [M(fx - 6 * S, fy), C(fx - 6 * S, fy - 8 * S, fx + 6 * S, fy - 8 * S, fx + 6 * S, fy)], 1.4);
        B.circle(Role.Arrow, fx, fy - 1.5 * S, 1.3 * S);
      }
      if (mb.marker) B.text(Role.Marker, barX + 3 * S, y + rowText + 11 * S, mb.marker, font * 0.9, 0).bold = true;
      {
        let tx = barX + 3 * S; const ty = y + rowTempo + 11 * S;
        let bpmHere = b.index === 0 ? score.tempo : barTempo(b);
        if (b.index === 0) {
          const t = barTempo(b);
          if (t > 0) bpmHere = t;
          if (score.tempoLabel) {
            B.text(Role.BarNumber, tx, ty, score.tempoLabel, font * 0.82, 0).bold = true;
            tx += utf8Length(score.tempoLabel) * font * 0.47 + 10 * S;
          }
        }
        if (bpmHere > 0) {
          const mark = `${QUARTER_NOTE} = ${Math.round(bpmHere)}`;
          B.text(Role.BarNumber, tx, ty, mark, font * 0.82, 0).bold = true;
          tx += (utf8Length(mark) - 2) * font * 0.47 + 10 * S;
        }
        if (b.index === 0 && track.capo > 0) {
          const capo = `Capo fret ${track.capo}`;
          B.text(Role.BarNumber, tx, ty, capo, font * 0.82, 0).bold = true;
          tx += capo.length * font * 0.47 + 10 * S;
        }
        for (const [tick, bpm] of midTempos(b)) {
          const at = lowerBound(b.keys, tick);
          const mx = at !== undefined ? b.slotX.get(at) : barEnd - 12 * S;
          B.text(Role.BarNumber, mx - 4 * S, ty, `${QUARTER_NOTE} = ${Math.round(bpm)}`, font * 0.82, 0).bold = true;
        }
        const feel = feelOf(b.index);
        if (feelOf(b.index - 1) !== feel) {
          // swing: two 8ths (or 16ths) = the uneven pair they are played as
          const six = feel.endsWith('16th');
          const even = six ? '♬' : '♫'; // beamed 16ths / 8ths
          const played = feel.startsWith('Triplet') ? '♩♪' : feel.startsWith('Dotted') ? '♪.♬' : feel.startsWith('Scottish') ? '♬♪.' : even;
          if (!feel) B.text(Role.BarNumber, tx, ty, 'straight', font * 0.75, 0).italic = true;
          else {
            B.text(Role.BarNumber, tx, ty, `(${even} = ${played})`, font * 0.9, 0).bold = true;
            if (feel.startsWith('Triplet')) B.text(Role.SmallText, tx + font * 3.05, ty - font * 0.75, '3', font * 0.55).bold = true;
          }
        }
      }
      if (mb.alternate) {
        // one bracket over all the bars of an ending in this row, numbered once
        const fromPrev = b !== sys.bars[0] && score.masterBars[b.index - 1].alternate === mb.alternate;
        const toNext = b !== sys.bars[sys.bars.length - 1] && score.masterBars[b.index + 1].alternate === mb.alternate;
        const ay = y + rowText + 2 * S; const x1 = toNext ? barEnd : barEnd - 4 * S;
        if (fromPrev) B.path(Role.Technique, [M(barX, ay), L(x1, ay)]);
        else {
          const nums = [];
          for (let k = 0; k < 8; k++) if (mb.alternate & (1 << k)) nums.push(`${k + 1}.`);
          B.path(Role.Technique, [M(barX + 2 * S, ay + 12 * S), L(barX + 2 * S, ay), L(x1, ay)]);
          B.text(Role.SmallText, barX + 6 * S, ay + 10 * S, nums.join(', '), font * 0.75, 0);
        }
      }
      out.bars[b.index] = { index: b.index, sys: si, x0: barX, x1: barEnd };
      b.sys = si;
      x = barEnd;
    }

    const rhythm = [];
    const pmSpans = []; const ringSpans = []; const harmSpans = []; const harmTypes = [];
    // bend in progress per string (voice 1): where its last segment ended and at what pitch
    const bendRun = Array.from({ length: strings }, () => ({ active: false, x: 0, level: 0 }));
    const bendTop = staffTop - 8 * S;
    const addSpan = (spans, bar, k, bx, len) => {
      const l = spans[spans.length - 1];
      if (l) {
        if (l.bar === bar && l.k === k - 1) { l.k = k; l.x1 = bx; return; }
        if (l.bar === bar - 1 && k === 0 && l.k === l.len - 1) { l.bar = bar; l.k = k; l.len = len; l.x1 = bx; return; }
      }
      spans.push({ bar, k, len, x0: bx, x1: bx });
    };

    for (const b of sys.bars) {
      const barX = out.bars[b.index].x0;
      b.bar.voices.forEach((voice, vi) => {
        voice.forEach((beat, k) => {
          let bx = b.slotX.has(beat.start) ? b.slotX.get(beat.start) : barX + 10 * S;
          if (beat.rest && beat.fullBar && voice.length === 1) bx = (barX + out.bars[b.index].x1) / 2; // a whole-bar rest sits in the middle of the bar
          const key = b.index * 4096 + k;
          if (vi === 0) out.beats.set(key, bx);
          if (beat.chord && o.chords) { const t = B.text(Role.Chord, bx, y + rowChord + 11 * S, beat.chord, font * 0.9); t.bold = t.italic = true; }
          if (beat.text) B.text(Role.BeatText, bx, y + rowText + 11 * S, beat.text, font * 0.8).italic = true;
          const mid = (staffTop + staffBottom) / 2;
          if (beat.rest) {
            if (vi !== 0) return;
            rhythm.push({ beat, x: bx, bar: b.index, rest: true });
            if (beat.fullBar || beat.value === 1) B.rect(Role.Rest, bx - 5 * S, mid - 4 * S, 10 * S, 3.5 * S, key);
            else if (beat.value === 2) B.rect(Role.Rest, bx - 5 * S, mid - 0.5 * S, 10 * S, 3.5 * S, key);
            else if (beat.value === 4) {
              B.path(Role.Rest, [M(bx - 2 * S, mid - 9 * S), L(bx + 2 * S, mid - 4 * S), L(bx - 2 * S, mid), L(bx + 2 * S, mid + 5 * S),
                C(bx - 2 * S, mid + 3 * S, bx - 3 * S, mid + 7 * S, bx + 1 * S, mid + 10 * S)], 1.8);
            } else {
              const flags = Math.max(1, Math.trunc(Math.log2(beat.value)) - 2);
              for (let i = 0; i < flags; i++) {
                const yy = mid - 6 * S + i * 5 * S;
                B.circle(Role.Rest, bx - 3 * S, yy, 1.6 * S);
                B.path(Role.Rest, [M(bx - 3 * S, yy + 1.4 * S), Q(bx, yy + 2.4 * S, bx + 3 * S, yy - 0.6 * S)], 1.2);
              }
              B.path(Role.Rest, [M(bx + 3 * S, mid - 6.5 * S), L(bx - 1 * S, mid - 6 * S + flags * 5 * S + 4 * S)], 1.2);
            }
            for (let d = 0; d < Math.min(beat.dots || 0, 2) && !beat.fullBar; d++) B.circle(Role.Rest, bx + (7 + d * 4) * S, mid - 3 * S, 1.4 * S);
            return;
          }
          if (vi === 0) rhythm.push({ beat, x: bx, bar: b.index, rest: false });
          // x where this beat's sound ends: the next beat of the voice, or the bar end
          let endX = out.bars[b.index].x1 - 4 * S;
          if (k + 1 < voice.length && b.slotX.has(voice[k + 1].start)) endX = b.slotX.get(voice[k + 1].start);
          // lowest and highest string this beat plays, for the brush arrow
          let sMin = strings; let sMax = -1;
          for (const n of beat.notes) if (n.string < strings) { sMin = Math.min(sMin, n.string); sMax = Math.max(sMax, n.string); }
          let beatW = 0; // widest fret number of the beat
          for (const n of beat.notes) {
            if (n.string >= strings) continue;
            const label = noteLabel(n);
            const ny = sy(n.string);
            const run = vi === 0 && o.techniques ? bendRun[n.string] : null;
            const continued = !!(run && n.tie && run.active);
            // Like Guitar Pro: a tied note is written again, in brackets, only at the start of a bar,
            // and not while it only carries a bend on.
            const hidden = continued || (n.tie && k > 0);
            const w = hidden ? 0 : label.length * digitW * (n.tie ? 0.85 : 1) + 4 * S;
            beatW = Math.max(beatW, w);
            if (!hidden) {
              gaps[n.string].push([bx - w / 2, bx + w / 2]);
              const soft = n.tie || n.ghost;
              const t = B.text(soft || vi ? Role.NoteSoft : Role.Note, bx, ny, label, soft ? font * 0.92 : font);
              t.bold = !soft;
              t.string = n.string;
              if (vi === 0) t.beatKey = key;
              const suffix = noteSuffix(n);
              if (suffix) {
                const sw = suffix.length * digitW * 0.7 + 2 * S;
                gaps[n.string].push([bx + w / 2, bx + w / 2 + sw]);
                B.text(Role.NoteSoft, bx + w / 2 + 1 * S, ny, suffix, font * 0.7, 0).string = n.string;
              }
            }
            if (!o.techniques) continue;
            if (n.grace) {
              // grace note: a small fret just before the note, joined by a slur or a slide
              const gl = n.grace.dead ? 'x' : String(n.grace.fret);
              const gw = gl.length * digitW * 0.75 + 2 * S;
              const gx = bx - Math.max(w, 8 * S) / 2 - 2 * S - gw / 2;
              gaps[n.string].push([gx - gw / 2, gx + gw / 2]);
              B.text(Role.NoteSoft, gx, ny, gl, font * 0.72).string = n.string;
              if (n.grace.slide) {
                const up = n.fret >= n.grace.fret;
                B.line(Role.Technique, gx + gw / 2, ny + (up ? 2 : -2) * S, bx - Math.max(w, 8 * S) / 2, ny + (up ? -2 : 2) * S, 1.1);
              }
              if (n.grace.hammer || n.grace.slide) {
                const ay = ny - gap * 0.5;
                B.path(Role.Technique, [M(gx, ay), Q((gx + bx) / 2, ay - 5 * S, bx, ay)], 1);
              }
            }
            // next note on this string (same row)
            let next = null; let nx = 0;
            for (let j = k + 1; j < voice.length && !next; j++) {
              for (const m of voice[j].notes) if (m.string === n.string) { next = m; nx = b.slotX.get(voice[j].start) ?? 0; }
              if (!next && voice[j].notes.length) break;
            }
            if (!next && b.index + 1 < bars.length) {
              const nb = bars[b.index + 1];
              const first = nb.bar.voices[0]?.[0];
              if (nb.sys === si && first) for (const m of first.notes) if (m.string === n.string) { next = m; nx = nb.slotX.get(first.start) ?? 0; }
            }
            if ((n.hammer || n.slide === 'legato') && next) {
              const ay = ny - gap * 0.55;
              B.path(Role.Technique, [M(bx + 3 * S, ay), Q((bx + nx) / 2, ay - 8 * S, nx - 3 * S, ay)]);
              if (n.hammer) B.text(Role.SmallText, (bx + nx) / 2, ay - 7 * S, next.fret >= n.fret ? 'H' : 'P', font * 0.68).bold = true;
            }
            if ((n.slide === 'shift' || n.slide === 'legato') && next) {
              const up = next.fret >= n.fret;
              const x1 = bx + w / 2 + 1 * S;
              const x2 = nx - (String(next.fret).length * digitW + 4 * S) / 2 - 1 * S;
              B.line(Role.Technique, x1, ny + (up ? 3 : -3) * S, x2, ny + (up ? -3 : 3) * S, 1.3);
            } else if (n.slide === 'outDown' || n.slide === 'outUp') {
              const d = n.slide === 'outUp' ? -1 : 1;
              B.line(Role.Technique, bx + w / 2, ny - d * 3 * S, bx + w / 2 + 10 * S, ny + d * 3 * S, 1.3);
            }
            if (n.slideIn) {
              const d = n.slideIn === 'below' ? 1 : -1;
              B.line(Role.Technique, bx - w / 2 - 10 * S, ny + d * 3 * S, bx - w / 2, ny - d * 3 * S, 1.3);
            }
            if (run && n.bend && n.bend.length) {
              // Guitar Pro style: rises end in an arrow above the staff with the amount,
              // holds are a dashed line up there, releases come back down to the note.
              const pts = bendShape(n.bend);
              let level = continued ? run.level : 0;
              let px = continued ? run.x : bx + w / 2;
              let py = level > 0 ? bendTop : ny - (continued ? 6 * S : 0);
              const span = Math.max(10 * S, endX - (bx + w / 2) - 3 * S);
              const xStart = continued ? bx : bx + w / 2;
              const X = (t) => xStart + Math.max(6 * S, t * span);
              const arrowUp = (ax, ay) => B.path(Role.Arrow, [M(ax - 3 * S, ay + 5 * S), L(ax, ay), L(ax + 3 * S, ay + 5 * S), Z()]);
              const arrowDown = (ax, ay) => B.path(Role.Arrow, [M(ax - 3 * S, ay - 5 * S), L(ax, ay), L(ax + 3 * S, ay - 5 * S), Z()]);
              const amount = (ax, v) => { B.text(Role.SmallText, ax, bendTop - 5 * S, bendLabel(v), font * 0.68).bold = true; };
              pts.forEach((p, pi) => {
                const v = p.semis;
                if (pi === 0) {
                  if (v > level + 0.01 && !continued) {
                    // pre-bend: straight up from the note
                    B.path(Role.Technique, [M(bx, ny - 7 * S), L(bx, bendTop + 4 * S)]);
                    arrowUp(bx, bendTop);
                    amount(bx, v);
                    px = bx; py = bendTop;
                  } else if (Math.abs(v - level) > 0.01 && continued) {
                    // the bend changed pitch across the tie: treat as a move from here
                    py = v > 0 ? bendTop : ny - 6 * S;
                  }
                  level = v;
                  return;
                }
                const x1 = Math.max(X(p.t), px + 6 * S);
                if (v > level) {
                  B.path(Role.Technique, [M(px, py), Q(x1, py, x1, bendTop + 4 * S)]);
                  arrowUp(x1, bendTop);
                  amount(x1, v);
                  py = bendTop;
                } else if (v < level) {
                  const y1 = v > 0 ? (bendTop + ny) / 2 : ny - 6 * S;
                  B.path(Role.Technique, [M(px, py), Q(x1, py, x1, y1 - 4 * S)]);
                  arrowDown(x1, y1);
                  if (v > 0) B.text(Role.SmallText, x1 + 3 * S, y1 - 2 * S, bendLabel(v), font * 0.62, 0).bold = true;
                  py = y1;
                } else if (v > 0) {
                  B.path(Role.Dashed, [M(px, bendTop + 1 * S), L(x1, bendTop + 1 * S)], 1);
                }
                px = x1; level = v;
              });
              run.active = true;
              run.x = px;
              run.level = level;
            } else if (run && continued && run.level > 0) {
              // tied note without its own bend points: the pitch holds
              B.path(Role.Dashed, [M(run.x, bendTop + 1 * S), L(endX - 3 * S, bendTop + 1 * S)], 1);
              run.x = endX - 3 * S;
            } else if (run && !n.tie) {
              run.active = false;
            }
          }
          if (!o.techniques || vi !== 0) return;
          const has = (pred) => beat.notes.some(pred);
          const laneY = (row) => y + row + 6.5 * S;
          const effEnd = Math.max(bx + 12 * S, endX - 4 * S); // where a beat-long effect line ends
          if (has((n) => n.palmMute)) addSpan(pmSpans, b.index, k, bx, voice.length);
          if (has((n) => n.letRing)) addSpan(ringSpans, b.index, k, bx, voice.length);
          const harm = beat.notes.find((n) => n.harmonic);
          if (harm) {
            if (harmTypes.length && harmTypes[harmTypes.length - 1] !== harm.harmonic) harmSpans.push({ bar: b.index, k, len: voice.length, x0: bx, x1: bx }); // a new kind starts its own label
            else addSpan(harmSpans, b.index, k, bx, voice.length);
            if (harmTypes.length < harmSpans.length) harmTypes.push(harm.harmonic);
          }
          if (beat.brush && sMax >= 0) {
            // strum: an arrow beside the chord, pointing the way the pick travels on the tab
            const ax = bx - Math.max(beatW, 8 * S) / 2 - 5 * S;
            const y0 = sy(sMin) - 5 * S; const y1 = sy(sMax) + 5 * S;
            B.line(Role.Technique, ax, y0 + (beat.brush === 'down' ? 5 : 0) * S, ax, y1 - (beat.brush === 'up' ? 5 : 0) * S, 1.2);
            if (beat.brush === 'down') B.path(Role.Arrow, [M(ax - 3 * S, y0 + 6 * S), L(ax, y0), L(ax + 3 * S, y0 + 6 * S), Z()]);
            else B.path(Role.Arrow, [M(ax - 3 * S, y1 - 6 * S), L(ax, y1), L(ax + 3 * S, y1 - 6 * S), Z()]);
          }
          const wide = !!beat.vibrato || has((n) => n.wideVibrato);
          if (wide || has((n) => n.vibrato)) {
            // a wavy line as long as the note; wide vibrato is taller and slower
            const vy = laneY(rowVib); const amp = (wide ? 6 : 4) * S; const step = (wide ? 6 : 4) * S;
            const ops = [M(bx - 2 * S, vy)];
            for (let cx = bx - 2 * S, i = 0; cx + step <= effEnd + 0.5 && i < 256; cx += step, i++) ops.push(Q(cx + step / 2, vy + (i % 2 ? amp : -amp) / 2, cx + step, vy));
            B.path(Role.Technique, ops, wide ? 1.5 : 1.2);
          }
          if (has((n) => trillOf(n) >= 0)) {
            const ty = laneY(rowTrill);
            const t = B.text(Role.SmallText, bx, ty, 'tr', font * 0.75);
            t.bold = t.italic = true;
            const ops = [M(bx + 7 * S, ty)];
            for (let cx = bx + 7 * S, i = 0; cx + 3 * S <= effEnd + 0.5 && i < 256; cx += 3 * S, i++) ops.push(Q(cx + 1.5 * S, ty + (i % 2 ? 2.5 : -2.5) * S, cx + 3 * S, ty));
            if (ops.length > 1) B.path(Role.Technique, ops, 1);
          }
          if (beat.tremolo > 0 && !beat.rest) {
            // tremolo picking: one slash per beam (8th, 16th, 32nd)
            const n = beat.tremolo <= 8 ? 1 : beat.tremolo <= 16 ? 2 : 3;
            const c = laneY(rowTrem);
            for (let i = 0; i < n; i++) {
              const oy = (i - (n - 1) / 2) * 3 * S;
              B.line(Role.Technique, bx - 3.5 * S, c + oy + 1.5 * S, bx + 3.5 * S, c + oy - 1.5 * S, 1.8);
            }
          }
          if (beat.whammy && beat.whammy.length) {
            // tremolo bar: the pitch drawn as a line around a centre, amounts in tones
            const c = y + rowWhammy + 15 * S; const span = effEnd - bx; const unit = 2.2 * S;
            const runStart = k === 0 || !(voice[k - 1].whammy && voice[k - 1].whammy.length);
            if (runStart) B.text(Role.SmallText, bx - 4 * S, y + rowWhammy + 4 * S, 'w/bar', font * 0.65, 0).italic = true;
            const ops = [];
            let lastV = 1e9;
            for (const p of beat.whammy) {
              const px = bx + p.t * span; const py = c - Math.min(9 * S, Math.max(-9 * S, p.semis * unit));
              ops.push(ops.length ? L(px, py) : M(px, py));
              if (Math.abs(p.semis - lastV) > 0.01 && p.semis !== 0) B.text(Role.SmallText, px + 2 * S, py + (p.semis < 0 ? 5 : -5) * S, whammyLabel(p.semis), font * 0.6, 0).bold = true;
              lastV = p.semis;
            }
            B.path(Role.Technique, ops, 1.3);
          }
          if (o.techniques && hasDyn && beat.notes.length) {
            let d = 0;
            for (const n of beat.notes) d = Math.max(d, dynOf(n) || defaultDyn);
            if (d !== dynBefore) { const t = B.text(Role.SmallText, bx, dynY, DYNAMICS[d] || '', font * 0.8); t.bold = t.italic = true; }
            dynBefore = d;
          }
          if (beat.fadeIn) {
            const c = laneY(rowFade);
            B.path(Role.Technique, [M(effEnd, c - 3.5 * S), L(bx - 2 * S, c), L(effEnd, c + 3.5 * S)]);
          }
          const tq = techniqueOf(beat);
          if (tq) B.text(Role.SmallText, bx, laneY(rowTSP), tq === 1 ? 'T' : tq === 2 ? 'S' : 'P', font * 0.75).bold = true;
          if (beat.pickStroke) {
            const pY = y + rowPick + 10.5 * S;
            if (beat.pickStroke === 'down') B.path(Role.Technique, [M(bx - 4 * S, pY), L(bx - 4 * S, pY - 7 * S), L(bx + 4 * S, pY - 7 * S), L(bx + 4 * S, pY)]);
            else B.path(Role.Technique, [M(bx - 3.5 * S, pY - 8 * S), L(bx, pY), L(bx + 3.5 * S, pY - 8 * S)]);
          }
          let accent = 0;
          for (const n of beat.notes) accent = Math.max(accent, accentOf(n));
          const staccato = has((n) => n.staccato);
          if (accent || staccato) {
            const c = laneY(rowAccent);
            if (accent === 2) B.path(Role.Technique, [M(bx - 3.5 * S, c + 3 * S), L(bx, c - 3.5 * S), L(bx + 3.5 * S, c + 3 * S)], 1.4); // heavy accent
            else if (accent === 1) B.path(Role.Technique, [M(bx - 4 * S, c - 2.5 * S), L(bx + 4 * S, c), L(bx - 4 * S, c + 2.5 * S)], 1.3);
            if (staccato) B.circle(Role.Arrow, accent ? bx + 7 * S : bx, c, 1.4 * S);
          }
        });
      });
    }

    const spans = (list, row, labelOf) => {
      const sY = y + row + 9 * S;
      list.forEach((sp, i) => {
        const label = labelOf(i);
        const lw = label.length * 5.2 * S;
        B.text(Role.SmallText, sp.x0 - 4 * S, sY, label, font * 0.7, 0).italic = true;
        if (sp.x1 > sp.x0 + lw) B.path(Role.Dashed, [M(sp.x0 + lw, sY - 3 * S), L(sp.x1 + 6 * S, sY - 3 * S), L(sp.x1 + 6 * S, sY + 1 * S)], 1);
      });
    };
    spans(pmSpans, rowPM, () => 'P.M.');
    spans(ringSpans, rowRing, () => 'let ring');
    spans(harmSpans, rowHarm, (i) => HARMONIC_NAMES[harmTypes[i]] || 'N.H.');

    if (o.rhythm) {
      const stemTop = rhythmTop; const stemBottom = rhythmTop + 22 * S; const beamGap = 4.5 * S;
      const levels = (v) => Math.max(0, Math.trunc(Math.log2(v)) - 2);
      const groups = [];
      let curId = '';
      let open = false;
      for (const it of rhythm) {
        const mb = bars[it.bar].mb;
        const groupTicks = mb.den === 8 ? (QUARTER * 3) / 2 : QUARTER;
        const gid = `${it.bar}:${Math.trunc(it.beat.start / groupTicks)}`;
        const beamable = !it.rest && it.beat.value >= 8;
        // the file's own beaming wins over the automatic grouping by beat
        const forced = it.beat.beam || 0;
        const sameBar = groups.length > 0 && groups[groups.length - 1][groups[groups.length - 1].length - 1].bar === it.bar;
        if (beamable && open && forced !== 1 && (gid === curId || (forced === 2 && sameBar))) groups[groups.length - 1].push(it);
        else {
          open = beamable;
          curId = gid;
          if (beamable) groups.push([it]);
        }
        if (it.rest || it.beat.value === 1) continue;
        const y0 = it.beat.value === 2 ? stemTop + 11 * S : stemTop;
        B.line(Role.Stem, it.x, y0, it.x, stemBottom, 1.2);
        for (let d = 0; d < Math.min(it.beat.dots || 0, 2); d++) B.circle(Role.Beam, it.x + (4 + d * 4) * S, stemBottom - 2 * S, 1.4 * S);
      }
      for (const g of groups) {
        if (g.length === 1) {
          const it = g[0];
          for (let l = 0; l < levels(it.beat.value); l++) {
            const yy = stemBottom - l * beamGap;
            B.path(Role.Stem, [M(it.x, yy), Q(it.x + 5 * S, yy - 3 * S, it.x + 7 * S, yy - 8 * S)]);
          }
          continue;
        }
        let maxL = 0;
        for (const it of g) maxL = Math.max(maxL, levels(it.beat.value));
        for (let l = 0; l < maxL; l++) {
          const yy = stemBottom - l * beamGap;
          for (let i = 0; i < g.length; i++) {
            const a = g[i];
            if (levels(a.beat.value) <= l) continue;
            let nb = i + 1 < g.length ? g[i + 1] : null;
            if (nb && l > 0 && nb.beat.beam === 3) nb = null; // secondary beams broken here
            if (nb && levels(nb.beat.value) > l) {
              B.rect(Role.Beam, a.x - 0.6 * S, yy - 2.6 * S, nb.x - a.x + 1.2 * S, 2.6 * S);
            } else {
              let p = i ? g[i - 1] : null;
              if (p && l > 0 && a.beat.beam === 3) p = null;
              if (!(p && levels(p.beat.value) > l)) {
                const bw = 6 * S;
                B.rect(Role.Beam, nb ? a.x : a.x - bw, yy - 2.6 * S, bw, 2.6 * S);
              }
            }
          }
        }
      }
      // tuplets: the number under each complete group; a bracket too when the group is not one beam
      const groupOf = new Map();
      groups.forEach((g, gi) => { for (const it of g) groupOf.set(it, gi); });
      for (let i = 0; i < rhythm.length;) {
        const first = rhythm[i];
        const fb = first.beat;
        if (tupNum(fb) < 2 || tupDen(fb) < 1) { i++; continue; }
        const span = beatTicks(fb.value, 0, 0, 0) * tupDen(fb);
        let sum = 0;
        let j = i;
        while (j < rhythm.length && sum < span && rhythm[j].bar === first.bar && tupNum(rhythm[j].beat) === tupNum(fb) && tupDen(rhythm[j].beat) === tupDen(fb)) sum += rhythm[j++].beat.ticks;
        const last = rhythm[j - 1];
        const g0 = groupOf.get(first);
        let oneBeam = g0 !== undefined && groups[g0].length === j - i;
        for (let m = i; m < j && oneBeam; m++) oneBeam = groupOf.get(rhythm[m]) === g0;
        const ty = stemBottom + 9 * S; const mid = (first.x + last.x) / 2;
        B.text(Role.SmallText, mid, ty, String(tupNum(fb)), font * 0.68).bold = true;
        if (!oneBeam && j - i > 1) {
          B.path(Role.Technique, [M(first.x - 3 * S, ty - 5 * S), L(first.x - 3 * S, ty), L(mid - 6 * S, ty)], 1);
          B.path(Role.Technique, [M(mid + 6 * S, ty), L(last.x + 3 * S, ty), L(last.x + 3 * S, ty - 5 * S)], 1);
        }
        i = j;
      }
    }

    // staff lines with gaps where notes sit, drawn underneath everything in this row
    const xEnd = x;
    const lines = [];
    for (let s = 0; s < strings; s++) {
      const gs = gaps[s].sort((a, b) => a[0] - b[0] || a[1] - b[1]);
      const ly = sy(s);
      let cur = lineStart;
      for (const [g0, g1] of gs) {
        if (g0 > cur) lines.push(new Prim(PrimType.Line, Role.Staff, cur, ly, g0, ly, 1, s));
        cur = Math.max(cur, g1);
      }
      if (cur < xEnd) lines.push(new Prim(PrimType.Line, Role.Staff, cur, ly, xEnd, ly, 1, s));
    }
    out.prims = out.prims.slice(0, staffStart).concat(lines, out.prims.slice(staffStart));
    sg.height = bottom - y;
    sg.x1 = xEnd;
    out.systems.push(sg);
    maxWidth = Math.max(maxWidth, xEnd + margin);
    y = bottom + 8 * S;
  });
  // whole pixels; float rounding (600.0001) must not add one
  out.width = Math.ceil((horizontal ? maxWidth : Math.max(o.width, maxWidth)) - 0.01);
  out.height = Math.ceil(y - 0.01);
  return out;
}

// ------------------------------------------------------------------ SVG

const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
const r1 = (x) => Math.round(x * 10) / 10;
const CLASS = ['st', 'bl', 'bt', 'rd', 'n', 'n soft', 'clef', 'sname', 'ts', 'bn', 'mk', 'cn', 'btx', 'sm', 'tq', 'tqf', 'dash', 'stem', 'beam', 'rest', 'arrow'];
const ANCHOR = ['start', 'middle', 'end'];

function pathData(ops) {
  return ops.map((op) => op[0] + op.slice(1).map(r1).join(' ')).join('');
}

// Only what differs from the defaults in tab.css is written: text is var(--fs), centred; strokes are 1.
function primSvg(p, font, attr = '') {
  const cls = CLASS[p.role] + (p.string >= 0 && (p.role === Role.Staff || p.role === Role.Note || p.role === Role.NoteSoft) ? ` s${p.string}` : '');
  const sw = p.role === Role.Staff || p.role === Role.BarLine || p.stroke === 1 ? '' : ` stroke-width="${r1(p.stroke)}"`;
  switch (p.type) {
    case PrimType.Line:
      return `<line class="${cls}"${attr}${sw} x1="${r1(p.x0)}" y1="${r1(p.y0)}" x2="${r1(p.x1)}" y2="${r1(p.y1)}"/>`;
    case PrimType.Rect:
      return `<rect class="${cls}"${attr} x="${r1(p.x0)}" y="${r1(p.y0)}" width="${r1(p.x1 - p.x0)}" height="${r1(p.y1 - p.y0)}"/>`;
    case PrimType.Circle:
      return `<circle class="${cls}"${attr} cx="${r1(p.x0)}" cy="${r1(p.y0)}" r="${r1(p.x1)}"/>`;
    case PrimType.Path:
      return `<path class="${cls}${p.role === Role.Arrow || p.role === Role.TechniqueFill ? '' : ' line'}"${attr}${sw} d="${pathData(p.path)}"/>`;
    default: {
      const style = (Math.abs(p.size - font) > 0.01 ? `font-size:${r1(p.size)}px;` : '') + (p.align !== 1 ? `text-anchor:${ANCHOR[p.align]};` : '')
        + (p.bold && p.role !== Role.Note ? 'font-weight:700;' : '') + (p.italic ? 'font-style:italic;' : '');
      return `<text class="${cls}${p.bold && p.role === Role.Note ? ' b' : ''}"${attr} x="${r1(p.x0)}" y="${r1(p.y0)}"${style ? ` style="${style}"` : ''}>${esc(p.text)}</text>`;
    }
  }
}

export function renderTrack(score, trackIndex = 0, options = {}) {
  const o = { ...DEFAULT_OPTIONS, ...options };
  const lay = layoutTrack(score, trackIndex, o);
  if (!score.tracks[trackIndex]) return { svg: '', width: 0, height: 0, bars: [], systems: [], beats: new Map() };
  const font = 12.5 * o.scale;
  // the notes of one beat go in one group, which the cursor lights up
  const groups = new Map();
  for (const p of lay.prims) if (p.beatKey >= 0 && p.type === PrimType.Text) (groups.get(p.beatKey) || groups.set(p.beatKey, []).get(p.beatKey)).push(p);
  const parts = [];
  for (const p of lay.prims) {
    if (p.beatKey >= 0 && p.type === PrimType.Text) {
      const g = groups.get(p.beatKey);
      if (g && g[0] === p) parts.push(`<g data-k="${p.beatKey}">${g.map((x) => primSvg(x, font)).join('')}</g>`);
      continue;
    }
    parts.push(primSvg(p, font, p.beatKey >= 0 ? ` data-k="${p.beatKey}"` : ''));
  }
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" class="tab" width="${lay.width}" height="${lay.height}" viewBox="0 0 ${lay.width} ${lay.height}" style="--fs:${r1(font)}px">${parts.join('')}</svg>`;
  return { svg, width: lay.width, height: lay.height, bars: lay.bars, systems: lay.systems, beats: lay.beats };
}
