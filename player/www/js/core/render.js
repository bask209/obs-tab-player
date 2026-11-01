// SPDX-License-Identifier: Apache-2.0
// Renders one track of a score as tablature SVG. All colours come from CSS classes
// (see tab.css), so themes change without re-rendering. Returns the SVG markup plus
// the geometry the cursor needs.

import { QUARTER, barTicks } from './model.js';

const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
const r1 = (x) => Math.round(x * 10) / 10;

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

export function renderTrack(score, trackIndex = 0, options = {}) {
  const o = { ...DEFAULT_OPTIONS, ...options };
  const track = score.tracks[trackIndex];
  if (!track) return { svg: '', width: 0, height: 0, bars: [], systems: [] };
  const S = o.scale;
  const strings = Math.max(1, track.tuning.length || 6);
  const gap = 13 * S; // distance between string lines
  const font = 12.5 * S;
  const digitW = font * 0.6;
  const margin = 8 * S;
  const clefW = o.labels === 'none' ? 6 * S : 26 * S;

  // ------------------------------------------------------------ horizontal metrics per bar
  const bars = score.masterBars.map((mb, i) => {
    const bar = track.bars[i] || { voices: [] };
    const len = barTicks(mb);
    // unique onsets across voices, each slot sized by the shortest beat starting there
    const slots = new Map();
    for (const v of bar.voices) {
      for (const b of v) {
        const cur = slots.get(b.start);
        const digits = Math.max(1, ...b.notes.map((n) => noteLabel(n).length));
        const w = Math.max(digits * digitW + 7 * S, (16 + 26 * Math.sqrt(Math.min(b.ticks, QUARTER * 4) / QUARTER)) * S * o.spacing);
        if (!cur || w < cur.w) slots.set(b.start, { tick: b.start, w: cur ? Math.max(cur.min, w) : w, min: digits * digitW + 7 * S });
      }
    }
    const list = [...slots.values()].sort((a, b) => a.tick - b.tick);
    if (!list.length) list.push({ tick: 0, w: 40 * S, min: 10 * S });
    const prev = score.masterBars[i - 1];
    const showTime = !prev || prev.num !== mb.num || prev.den !== mb.den;
    const pre = 12 * S + (showTime ? 24 * S : 0) + (mb.repeatStart ? 14 * S : 0);
    const post = 6 * S + (mb.repeatPlays ? 10 * S : 0);
    const content = list.reduce((s, x) => s + x.w, 0);
    return { index: i, mb, bar, len, slots: list, showTime, pre, post, content, natural: pre + content + post };
  });

  // ------------------------------------------------------------ systems (rows)
  const systems = [];
  if (o.mode === 'horizontal') {
    systems.push({ bars: bars.slice(), stretch: 1 });
  } else {
    const avail = o.width - margin * 2 - clefW;
    let row = [];
    let w = 0;
    for (const b of bars) {
      if (row.length && w + b.natural > avail) { systems.push({ bars: row }); row = []; w = 0; }
      row.push(b);
      w += b.natural;
    }
    if (row.length) systems.push({ bars: row, last: true });
    for (const sys of systems) {
      const nat = sys.bars.reduce((s, b) => s + b.natural, 0);
      const fixed = sys.bars.reduce((s, b) => s + b.pre + b.post, 0);
      const fill = sys.last && nat < avail * 0.7 ? 1 : (avail - fixed) / Math.max(1, nat - fixed);
      // never squeeze a beat below the room its fret numbers need
      const floor = Math.max(...sys.bars.flatMap((bar) => bar.slots.map((sl) => sl.min / sl.w)));
      sys.stretch = Math.max(floor, fill);
    }
  }

  // ------------------------------------------------------------ vertical layout
  const out = [];
  const geo = { bars: [], systems: [], beats: new Map() };
  let y = margin;
  let maxWidth = 0;
  const lastBar = bars.length - 1;

  for (const sys of systems) {
    const has = (pred) => sys.bars.some((b) => b.bar.voices.some((v) => v.some(pred)));
    const hasChords = o.chords && has((b) => b.chord);
    const hasText = sys.bars.some((b) => b.mb.marker || b.mb.alternate || (b.mb.repeatPlays > 2)) || has((b) => b.text);
    const hasBend = o.techniques && has((b) => b.notes.some((n) => n.bend));
    const hasSpan = o.techniques && has((b) => b.notes.some((n) => n.palmMute || n.letRing));
    const hasTech = o.techniques && has((b) => b.pickStroke || b.vibrato || b.notes.some((n) => n.vibrato));
    let top = 4 * S;
    const rowChord = top; if (hasChords) top += 16 * S;
    const rowText = top; if (hasText) top += 16 * S;
    const rowSpan = top; if (hasSpan) top += 13 * S;
    const rowTech = top; if (hasTech) top += 13 * S;
    if (hasBend) top += 18 * S;
    if (o.barNumbers && !hasText && !hasChords) top += 6 * S;
    const staffTop = y + top + 6 * S;
    const staffH = gap * (strings - 1);
    const staffBottom = staffTop + staffH;
    const rhythmTop = staffBottom + 7 * S;
    const bottom = staffBottom + (o.rhythm ? 34 * S : 10 * S);
    const sy = (s) => staffTop + s * gap;

    const parts = { lines: [], bars: [], notes: [], rhythm: [], tech: [], text: [] };
    const gaps = Array.from({ length: strings }, () => []); // per string: [x0, x1] gaps in the line
    let x = margin;

    // clef / string names
    if (o.labels === 'tab' && strings >= 3) {
      const mid = (staffTop + staffBottom) / 2;
      const step = Math.min(gap * 1.15, staffH / 3);
      ['T', 'A', 'B'].forEach((ch, k) => parts.text.push(`<text class="clef" x="${r1(x + 9 * S)}" y="${r1(mid + (k - 1) * step)}">${ch}</text>`));
      for (let s = 0; s < strings; s++) gaps[s].push([x + 2 * S, x + 16 * S]);
    } else if (o.labels === 'names') {
      for (let s = 0; s < strings; s++) {
        const name = NOTE_NAMES[((track.tuning[s] ?? 40) % 12 + 12) % 12];
        parts.text.push(`<text class="sname" x="${r1(x + 8 * S)}" y="${r1(sy(s))}">${name}</text>`);
      }
    }
    const lineStart = x + (o.labels === 'names' ? 18 * S : 0);
    x += clefW;
    parts.bars.push(`<line class="bl" x1="${r1(lineStart)}" y1="${r1(staffTop)}" x2="${r1(lineStart)}" y2="${r1(staffBottom)}"/>`);

    const sysGeo = { y, top: staffTop, bottom: staffBottom, height: 0, x0: lineStart };
    const pmSpans = [];
    const ringSpans = [];
    const rhythmBeats = [];

    for (const b of sys.bars) {
      const mb = b.mb;
      const barX = x;
      let cx = barX;
      if (mb.repeatStart) {
        parts.bars.push(`<rect class="bt" x="${r1(cx + 1 * S)}" y="${r1(staffTop)}" width="${r1(3 * S)}" height="${r1(staffH)}"/>`);
        parts.bars.push(`<line class="bl" x1="${r1(cx + 7 * S)}" y1="${r1(staffTop)}" x2="${r1(cx + 7 * S)}" y2="${r1(staffBottom)}"/>`);
        repeatDots(parts, cx + 11 * S, staffTop, staffBottom, gap, S);
        cx += 14 * S;
      }
      if (b.showTime) {
        const tx = cx + 13 * S;
        const mid = (staffTop + staffBottom) / 2;
        parts.text.push(`<text class="ts" x="${r1(tx)}" y="${r1(mid - staffH * 0.22)}" font-size="${r1(Math.min(staffH * 0.42, 24 * S))}">${mb.num}</text>`);
        parts.text.push(`<text class="ts" x="${r1(tx)}" y="${r1(mid + staffH * 0.22)}" font-size="${r1(Math.min(staffH * 0.42, 24 * S))}">${mb.den}</text>`);
        for (let s = 0; s < strings; s++) gaps[s].push([tx - 8 * S, tx + 8 * S]);
        cx += 24 * S;
      }
      cx += 12 * S;
      // slot x positions
      const slotX = new Map();
      for (const sl of b.slots) {
        slotX.set(sl.tick, cx + sl.min / 2);
        cx += sl.w * sys.stretch;
      }
      cx += b.post - (mb.repeatPlays ? 10 * S : 0);
      if (mb.repeatPlays) {
        repeatDots(parts, cx - 1 * S, staffTop, staffBottom, gap, S);
        cx += 4 * S;
        parts.bars.push(`<line class="bl" x1="${r1(cx)}" y1="${r1(staffTop)}" x2="${r1(cx)}" y2="${r1(staffBottom)}"/>`);
        parts.bars.push(`<rect class="bt" x="${r1(cx + 3 * S)}" y="${r1(staffTop)}" width="${r1(3 * S)}" height="${r1(staffH)}"/>`);
        cx += 6 * S;
        if (mb.repeatPlays > 2) parts.text.push(`<text class="rep" x="${r1(cx - 4 * S)}" y="${r1(y + rowText + 10 * S)}">×${mb.repeatPlays}</text>`);
      } else if (b.index === lastBar) {
        parts.bars.push(`<line class="bl" x1="${r1(cx - 5 * S)}" y1="${r1(staffTop)}" x2="${r1(cx - 5 * S)}" y2="${r1(staffBottom)}"/>`);
        parts.bars.push(`<rect class="bt" x="${r1(cx - 2 * S)}" y="${r1(staffTop)}" width="${r1(3 * S)}" height="${r1(staffH)}"/>`);
        cx += 1 * S;
      } else {
        parts.bars.push(`<line class="bl" x1="${r1(cx)}" y1="${r1(staffTop)}" x2="${r1(cx)}" y2="${r1(staffBottom)}"/>`);
        if (mb.doubleBar) parts.bars.push(`<line class="bl" x1="${r1(cx - 3 * S)}" y1="${r1(staffTop)}" x2="${r1(cx - 3 * S)}" y2="${r1(staffBottom)}"/>`);
      }
      const barEnd = cx;

      // bar number, marker, alternate ending
      if (o.barNumbers) parts.text.push(`<text class="bn" x="${r1(barX + 2 * S)}" y="${r1(staffTop - 5 * S)}">${b.index + 1}</text>`);
      if (mb.marker) parts.text.push(`<text class="mk" x="${r1(barX + 3 * S)}" y="${r1(y + rowText + 11 * S)}">${esc(mb.marker)}</text>`);
      if (mb.alternate) {
        const nums = [];
        for (let k = 0; k < 8; k++) if (mb.alternate & (1 << k)) nums.push(k + 1);
        const ay = y + rowText + 2 * S;
        parts.tech.push(`<path class="tq" d="M${r1(barX + 2 * S)} ${r1(ay + 12 * S)}V${r1(ay)}H${r1(barEnd - 4 * S)}"/>`);
        parts.text.push(`<text class="alt" x="${r1(barX + 6 * S)}" y="${r1(ay + 10 * S)}">${nums.join('., ')}.</text>`);
      }

      geo.bars[b.index] = { index: b.index, sys: geo.systems.length, x0: barX, x1: barEnd };
      b.pos = { sys: geo.systems.length, slotX };
      x = barEnd;
    }

    for (const b of sys.bars) {
      const slotX = b.pos.slotX;
      const barX = geo.bars[b.index].x0;
      b.bar.voices.forEach((voice, vi) => {
        voice.forEach((beat, k) => {
          const bx = slotX.get(beat.start) ?? barX + 10 * S;
          if (vi === 0) geo.beats.set(b.index * 4096 + k, bx);
          const keyAttr = vi === 0 ? ` data-k="${b.index * 4096 + k}"` : '';
          if (beat.chord && o.chords) parts.text.push(`<text class="cn" x="${r1(bx)}" y="${r1(y + rowChord + 11 * S)}">${esc(beat.chord)}</text>`);
          if (beat.text) parts.text.push(`<text class="bt-text" x="${r1(bx)}" y="${r1(y + rowText + 11 * S)}">${esc(beat.text)}</text>`);
          if (beat.rest) {
            if (vi === 0) {
              parts.notes.push(restGlyph(beat, bx, (staffTop + staffBottom) / 2, S, keyAttr));
              rhythmBeats.push({ beat, x: bx, bar: b.index, rest: true });
            }
            return;
          }
          if (vi === 0) rhythmBeats.push({ beat, x: bx, bar: b.index });
          const g = [];
          for (const n of beat.notes) {
            if (n.string >= strings) continue;
            const label = noteLabel(n);
            const ny = sy(n.string);
            const w = label.length * digitW * (n.tie ? 0.85 : 1) + 4 * S;
            gaps[n.string].push([bx - w / 2, bx + w / 2]);
            const cls = `n s${n.string}${n.tie ? ' tie' : ''}${n.ghost ? ' ghost' : ''}${n.dead ? ' dead' : ''}${vi ? ' v2' : ''}`;
            g.push(`<text class="${cls}" x="${r1(bx)}" y="${r1(ny)}">${esc(label)}</text>`);
            if (!o.techniques) continue;
            // legato arcs, slides, bends
            const nxt = nextOnString(b, voice, k, n.string, bars, geo.systems.length);
            if (n.hammer && nxt) {
              const x2 = nxt.x;
              const up = nxt.note.fret >= n.fret;
              const ay = ny - gap * 0.55;
              parts.tech.push(`<path class="arc" d="M${r1(bx + 3 * S)} ${r1(ay)}Q${r1((bx + x2) / 2)} ${r1(ay - 8 * S)} ${r1(x2 - 3 * S)} ${r1(ay)}"/>`);
              parts.tech.push(`<text class="hp" x="${r1((bx + x2) / 2)}" y="${r1(ay - 7 * S)}">${up ? 'H' : 'P'}</text>`);
            }
            if ((n.slide === 'shift' || n.slide === 'legato') && nxt) {
              const up = nxt.note.fret >= n.fret;
              const x1 = bx + w / 2 + 1 * S;
              const x2 = nxt.x - (String(nxt.note.fret).length * digitW + 4 * S) / 2 - 1 * S;
              parts.tech.push(`<line class="sl" x1="${r1(x1)}" y1="${r1(ny + (up ? 3 : -3) * S)}" x2="${r1(x2)}" y2="${r1(ny + (up ? -3 : 3) * S)}"/>`);
            } else if (n.slide === 'outDown' || n.slide === 'outUp') {
              const d = n.slide === 'outUp' ? -1 : 1;
              parts.tech.push(`<line class="sl" x1="${r1(bx + w / 2)}" y1="${r1(ny - d * 3 * S)}" x2="${r1(bx + w / 2 + 10 * S)}" y2="${r1(ny + d * 3 * S)}"/>`);
            }
            if (n.slideIn) {
              const d = n.slideIn === 'below' ? 1 : -1;
              parts.tech.push(`<line class="sl" x1="${r1(bx - w / 2 - 10 * S)}" y1="${r1(ny + d * 3 * S)}" x2="${r1(bx - w / 2)}" y2="${r1(ny - d * 3 * S)}"/>`);
            }
            if (n.bend && n.bend.length) {
              const peak = Math.max(...n.bend.map((p) => p.semis));
              const first = n.bend[0].semis;
              const last = n.bend[n.bend.length - 1].semis;
              const topY = staffTop - 8 * S;
              const x1 = bx + w / 2;
              const x2 = bx + Math.max(14 * S, w / 2 + 10 * S);
              if (first < peak - 0.05) {
                // bend up from the note
                parts.tech.push(`<path class="bend" d="M${r1(x1)} ${r1(ny)}Q${r1(x2)} ${r1(ny)} ${r1(x2)} ${r1(topY + 4 * S)}"/>`);
              } else {
                // pre-bend: straight up
                parts.tech.push(`<path class="bend" d="M${r1(bx)} ${r1(ny - 6 * S)}V${r1(topY + 4 * S)}"/>`);
              }
              const ax = first < peak - 0.05 ? x2 : bx;
              parts.tech.push(`<path class="arrow" d="M${r1(ax - 3 * S)} ${r1(topY + 5 * S)}L${r1(ax)} ${r1(topY)}L${r1(ax + 3 * S)} ${r1(topY + 5 * S)}Z"/>`);
              parts.tech.push(`<text class="bendl" x="${r1(ax)}" y="${r1(topY - 5 * S)}">${bendLabel(peak)}</text>`);
              if (last < peak - 0.05) {
                // release back down
                const rx = ax + 10 * S;
                parts.tech.push(`<path class="bend dashless" d="M${r1(ax + 1 * S)} ${r1(topY + 2 * S)}Q${r1(rx)} ${r1(topY + 2 * S)} ${r1(rx)} ${r1(ny - 4 * S)}"/>`);
                parts.tech.push(`<path class="arrow" d="M${r1(rx - 3 * S)} ${r1(ny - 9 * S)}L${r1(rx)} ${r1(ny - 4 * S)}L${r1(rx + 3 * S)} ${r1(ny - 9 * S)}Z"/>`);
              }
            }
            if (n.staccato) parts.tech.push(`<circle class="dot" cx="${r1(bx)}" cy="${r1(ny + 7 * S)}" r="${r1(1.4 * S)}"/>`);
          }
          parts.notes.push(`<g${keyAttr}>${g.join('')}</g>`);
          if (!o.techniques || vi !== 0) return;
          const tY = y + rowTech + 10 * S;
          if (beat.notes.some((n) => n.palmMute)) addSpan(pmSpans, b.index, k, bx, voice);
          if (beat.notes.some((n) => n.letRing)) addSpan(ringSpans, b.index, k, bx, voice);
          if (beat.notes.some((n) => n.vibrato) || beat.vibrato) {
            parts.tech.push(`<path class="vib" d="${wave(bx - 3 * S, tY - 3 * S, 18 * S, S)}"/>`);
          }
          if (beat.notes.some((n) => n.accent)) parts.tech.push(`<path class="tq" d="M${r1(bx - 4 * S)} ${r1(staffTop - 9 * S)}L${r1(bx + 4 * S)} ${r1(staffTop - 6.5 * S)}L${r1(bx - 4 * S)} ${r1(staffTop - 4 * S)}"/>`);
          if (beat.pickStroke === 'down') parts.tech.push(`<path class="pick" d="M${r1(bx - 4 * S)} ${r1(tY)}V${r1(tY - 7 * S)}H${r1(bx + 4 * S)}V${r1(tY)}"/>`);
          else if (beat.pickStroke === 'up') parts.tech.push(`<path class="pick" d="M${r1(bx - 3.5 * S)} ${r1(tY - 8 * S)}L${r1(bx)} ${r1(tY)}L${r1(bx + 3.5 * S)} ${r1(tY - 8 * S)}"/>`);
        });
      });
    }

    // technique spans (P.M., let ring)
    const tY = y + rowSpan + 9 * S;
    for (const sp of pmSpans) spanMarkup(parts, 'P.M.', sp, tY, S);
    for (const sp of ringSpans) spanMarkup(parts, 'let ring', sp, tY, S);

    // rhythm (voice 1 only)
    if (o.rhythm) drawRhythm(parts, rhythmBeats, sys, bars, rhythmTop, S);

    // staff lines with gaps where notes sit
    const xEnd = x;
    for (let s = 0; s < strings; s++) {
      const ly = r1(sy(s));
      const gs = gaps[s].sort((a, b) => a[0] - b[0]);
      let d = '';
      let cur = lineStart;
      for (const [g0, g1] of gs) {
        if (g0 > cur) d += `M${r1(cur)} ${ly}H${r1(g0)}`;
        cur = Math.max(cur, g1);
      }
      if (cur < xEnd) d += `M${r1(cur)} ${ly}H${r1(xEnd)}`;
      parts.lines.push(`<path class="st s${s}" d="${d}"/>`);
    }

    out.push(`<g class="sys">${parts.lines.join('')}${parts.bars.join('')}${parts.tech.join('')}${parts.rhythm.join('')}${parts.notes.join('')}${parts.text.join('')}</g>`);
    sysGeo.height = bottom - y;
    sysGeo.x1 = xEnd;
    geo.systems.push(sysGeo);
    maxWidth = Math.max(maxWidth, xEnd + margin);
    y = bottom + 8 * S;
  }

  const width = o.mode === 'horizontal' ? Math.ceil(maxWidth) : Math.ceil(Math.max(o.width, maxWidth));
  const height = Math.ceil(y);
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" class="tab" width="${width}" height="${height}" viewBox="0 0 ${width} ${height}" style="--fs:${r1(font)}px">${out.join('')}</svg>`;
  return { svg, width, height, bars: geo.bars, systems: geo.systems, beats: geo.beats };
}

export function noteLabel(n) {
  if (n.dead) return 'x';
  if (n.harmonic === 'natural') return `<${n.fret}>`;
  if (n.harmonic === 'artificial' || n.harmonic === 'pinch') return `${n.fret}`;
  if (n.ghost || n.tie) return `(${n.fret})`;
  return String(n.fret);
}

function bendLabel(semis) {
  const steps = Math.round(semis * 2) / 4; // whole tones in quarters
  if (steps <= 0) return '';
  if (steps === 0.25) return '¼';
  if (steps === 0.5) return '½';
  if (steps === 1) return 'full';
  const whole = Math.floor(steps);
  const frac = steps - whole;
  return `${whole}${frac === 0.5 ? '½' : frac === 0.25 ? '¼' : frac === 0.75 ? '¾' : ''}`;
}

function repeatDots(parts, x, top, bottom, gap, S) {
  const mid = (top + bottom) / 2;
  const off = Math.max(gap * 0.5, (bottom - top) * 0.15);
  parts.bars.push(`<circle class="rd" cx="${r1(x)}" cy="${r1(mid - off)}" r="${r1(1.9 * S)}"/><circle class="rd" cx="${r1(x)}" cy="${r1(mid + off)}" r="${r1(1.9 * S)}"/>`);
}

function wave(x, y, w, S) {
  let d = `M${r1(x)} ${r1(y)}`;
  const step = 4 * S;
  for (let i = 0; i < Math.floor(w / step); i++) d += `q${r1(step / 2)} ${r1(i % 2 ? 3 * S : -3 * S)} ${r1(step)} 0`;
  return d;
}

function addSpan(spans, bar, k, x, voice) {
  const last = spans[spans.length - 1];
  if (last && last.bar === bar && last.k === k - 1) { last.k = k; last.x1 = x; return; }
  if (last && last.bar === bar - 1 && k === 0 && last.k === last.len - 1) { last.bar = bar; last.k = k; last.len = voice.length; last.x1 = x; return; }
  spans.push({ bar, k, len: voice.length, x0: x, x1: x });
}

function spanMarkup(parts, label, sp, y, S) {
  const lw = label.length * 5.2 * S;
  parts.text.push(`<text class="span" x="${r1(sp.x0 - 4 * S)}" y="${r1(y)}">${label}</text>`);
  if (sp.x1 > sp.x0 + lw) parts.tech.push(`<path class="dash" d="M${r1(sp.x0 + lw)} ${r1(y - 3 * S)}H${r1(sp.x1 + 6 * S)}V${r1(y + 1 * S)}"/>`);
}

// Next note on the same string (may continue into the next bar of the same row).
function nextOnString(b, voice, k, string, bars, sysIndex) {
  for (let j = k + 1; j < voice.length; j++) {
    const n = voice[j].notes.find((x) => x.string === string);
    if (n) return { note: n, x: b.pos.slotX.get(voice[j].start) };
    if (voice[j].notes.length) return null;
  }
  const nb = bars[b.index + 1];
  const first = nb?.bar.voices[0]?.[0];
  if (first && nb.pos && nb.pos.sys === sysIndex) {
    const n = first.notes.find((x) => x.string === string);
    if (n) return { note: n, x: nb.pos.slotX.get(first.start) };
  }
  return null;
}

function restGlyph(beat, x, mid, S, keyAttr) {
  const v = beat.value;
  if (beat.fullBar || v === 1) return `<rect${keyAttr} class="rest" x="${r1(x - 5 * S)}" y="${r1(mid - 4 * S)}" width="${r1(10 * S)}" height="${r1(3.5 * S)}"/>`;
  if (v === 2) return `<rect${keyAttr} class="rest" x="${r1(x - 5 * S)}" y="${r1(mid - 0.5 * S)}" width="${r1(10 * S)}" height="${r1(3.5 * S)}"/>`;
  if (v === 4) {
    return `<path${keyAttr} class="rest-q" d="M${r1(x - 2 * S)} ${r1(mid - 9 * S)}l${r1(4 * S)} ${r1(5 * S)}l${r1(-4 * S)} ${r1(4 * S)}l${r1(4 * S)} ${r1(5 * S)}c${r1(-4 * S)} ${r1(-2 * S)} ${r1(-5 * S)} ${r1(2 * S)} ${r1(-1 * S)} ${r1(5 * S)}"/>`;
  }
  const flags = Math.max(1, Math.log2(v) - 2);
  let d = '';
  for (let i = 0; i < flags; i++) {
    const yy = mid - 6 * S + i * 5 * S;
    d += `M${r1(x - 3 * S)} ${r1(yy)}a${r1(1.6 * S)} ${r1(1.6 * S)} 0 1 0 0.1 0Z`;
    d += `M${r1(x - 3 * S)} ${r1(yy + 1.4 * S)}q${r1(3 * S)} ${r1(1 * S)} ${r1(6 * S)} ${r1(-2 * S)}`;
  }
  d += `M${r1(x + 3 * S)} ${r1(mid - 6.5 * S)}L${r1(x - 1 * S)} ${r1(mid - 6 * S + flags * 5 * S + 4 * S)}`;
  return `<path${keyAttr} class="rest-e" d="${d}"/>`;
}

// Stems, beams, dots and tuplet numbers under the staff.
function drawRhythm(parts, items, sys, bars, top, S) {
  const stemTop = top;
  const stemBottom = top + 22 * S;
  const beamGap = 4.5 * S;
  const levels = (v) => Math.max(0, Math.log2(v) - 2); // 8th = 1, 16th = 2 ...
  // group beamable beats: same bar, same beat-group, no rests in between
  const groups = [];
  let cur = null;
  for (const it of items) {
    const b = it.beat;
    const mb = bars[it.bar].mb;
    const groupTicks = mb.den === 8 && mb.num % 3 === 0 ? QUARTER * 1.5 : mb.den === 8 ? QUARTER * 1.5 : QUARTER;
    const gid = `${it.bar}:${Math.floor(b.start / groupTicks)}`;
    if (!it.rest && b.value >= 8 && cur && cur.gid === gid) cur.items.push(it);
    else {
      cur = !it.rest && b.value >= 8 ? { gid, items: [it] } : null;
      if (cur) groups.push(cur);
    }
    if (it.rest) continue;
    if (b.value === 1) continue;
    const y0 = b.value === 2 ? stemTop + 11 * S : stemTop;
    parts.rhythm.push(`<line class="stem" x1="${r1(it.x)}" y1="${r1(y0)}" x2="${r1(it.x)}" y2="${r1(stemBottom)}"/>`);
    if (b.dots) parts.rhythm.push(`<circle class="rdot" cx="${r1(it.x + 4 * S)}" cy="${r1(stemBottom - 2 * S)}" r="${r1(1.4 * S)}"/>`);
  }
  for (const g of groups) {
    const its = g.items;
    if (its.length === 1) {
      // flags as short beams
      const it = its[0];
      for (let l = 0; l < levels(it.beat.value); l++) {
        const y = stemBottom - l * beamGap;
        parts.rhythm.push(`<path class="flag" d="M${r1(it.x)} ${r1(y)}q${r1(5 * S)} ${r1(-3 * S)} ${r1(7 * S)} ${r1(-8 * S)}"/>`);
      }
      continue;
    }
    const maxL = Math.max(...its.map((i) => levels(i.beat.value)));
    for (let l = 0; l < maxL; l++) {
      const y = stemBottom - l * beamGap;
      for (let i = 0; i < its.length; i++) {
        const a = its[i];
        if (levels(a.beat.value) <= l) continue;
        const b = its[i + 1];
        if (b && levels(b.beat.value) > l) {
          parts.rhythm.push(`<rect class="beam" x="${r1(a.x - 0.6 * S)}" y="${r1(y - 2.6 * S)}" width="${r1(b.x - a.x + 1.2 * S)}" height="${r1(2.6 * S)}"/>`);
        } else {
          const p = its[i - 1];
          if (!(p && levels(p.beat.value) > l)) {
            const dir = b ? 1 : -1;
            const w = 6 * S;
            const x = dir > 0 ? a.x : a.x - w;
            parts.rhythm.push(`<rect class="beam" x="${r1(x)}" y="${r1(y - 2.6 * S)}" width="${r1(w)}" height="${r1(2.6 * S)}"/>`);
          }
        }
      }
    }
    const tup = its[0].beat.tuplet;
    if (tup && its.every((i) => i.beat.tuplet && i.beat.tuplet[0] === tup[0])) {
      const mid = (its[0].x + its[its.length - 1].x) / 2;
      parts.rhythm.push(`<text class="tup" x="${r1(mid)}" y="${r1(stemBottom + 9 * S)}">${tup[0]}</text>`);
    }
  }
  // tuplets on lone beats (quarter triplets etc.)
  for (const it of items) {
    if (it.rest || !it.beat.tuplet || it.beat.value >= 8) continue;
    if (it.beat.start % (QUARTER * 2) === 0 || it.beat === items[0]?.beat) {
      parts.rhythm.push(`<text class="tup" x="${r1(it.x)}" y="${r1(stemBottom + 9 * S)}">${it.beat.tuplet[0]}</text>`);
    }
  }
}
