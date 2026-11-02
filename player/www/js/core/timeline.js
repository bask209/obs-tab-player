// SPDX-License-Identifier: Apache-2.0
// Turns a score into a flat, time-sorted playback timeline (seconds at speed 1).

import { QUARTER, barTicks, naturalHarmonicOffset } from './model.js';

// Bar play order with repeats and alternate endings expanded.
export function playOrder(masterBars) {
  const order = [];
  const reached = new Map(); // repeat-close bar -> times played so far
  let start = 0;
  let pass = 1;
  let jumped = false;
  for (let i = 0, guard = 0; i < masterBars.length && guard < 20000; guard++) {
    const mb = masterBars[i];
    if (mb.repeatStart && !jumped) { start = i; pass = 1; }
    jumped = false;
    if (mb.alternate && !(mb.alternate & (1 << (pass - 1)))) { i++; continue; }
    order.push(i);
    if (mb.repeatPlays > 1) {
      const n = reached.get(i) || 1;
      if (n < mb.repeatPlays) {
        reached.set(i, n + 1);
        pass = n + 1;
        i = start;
        jumped = true;
        continue;
      }
      reached.delete(i);
      start = i + 1;
    }
    i++;
  }
  return order;
}

const MAX_RING = 4; // seconds

export function buildTimeline(score, opts = {}) {
  const { displayTrack = 0, playTracks = null } = opts;
  const order = playOrder(score.masterBars);
  const bars = []; // { index, start, end, startTick, bpm }
  const notes = [];
  const beats = []; // cursor stops for the display track
  const clicks = [];

  let bpm = score.tempo || 120;
  let time = 0;
  let tick = 0;

  // Tempo changes from mix tables (GP3-5) live on beats. Collect them per bar once.
  const beatTempos = score.masterBars.map(() => []);
  for (const track of score.tracks) {
    track.bars.forEach((bar, bi) => {
      for (const v of bar.voices) for (const b of v) if (b.tempo) beatTempos[bi].push({ tick: b.start, bpm: b.tempo });
    });
  }

  const tracks = score.tracks.map((t, i) => ({ t, i, play: !t.percussion && (!playTracks || playTracks.includes(i)) }));
  const lastOnString = tracks.map(() => []); // per track: string -> note event

  for (let occ = 0; occ < order.length; occ++) {
    const bi = order[occ];
    const mb = score.masterBars[bi];
    const len = barTicks(mb);
    // tempo map inside this bar
    const changes = [...beatTempos[bi], ...(mb.tempoChanges || []).map((c) => ({ tick: c.ratio * len, bpm: c.bpm }))]
      .sort((a, b) => a.tick - b.tick);
    const startBpm = bpm;
    const secAt = (tk) => {
      let t = 0; let last = 0; let cur = startBpm;
      for (const c of changes) {
        if (c.tick >= tk) break;
        t += ((c.tick - last) * 60) / (cur * QUARTER);
        last = c.tick; cur = c.bpm;
      }
      return t + ((tk - last) * 60) / (cur * QUARTER);
    };
    const barStart = time;
    const barDur = secAt(len);
    for (const c of changes) bpm = c.bpm;
    bars.push({ index: bi, start: barStart, end: barStart + barDur, startTick: tick, bpm: startBpm });

    // metronome
    const compound = mb.den === 8 && mb.num % 3 === 0 && mb.num > 3;
    const clickCount = compound ? mb.num / 3 : mb.num;
    const clickTicks = len / clickCount;
    for (let k = 0; k < clickCount; k++) clicks.push({ t: barStart + secAt(k * clickTicks), down: k === 0 });

    for (const { t: track, i: ti, play } of tracks) {
      const bar = track.bars[bi];
      if (!bar) continue;
      bar.voices.forEach((voice, vi) => {
        voice.forEach((beat, k) => {
          const [from, to] = feelTicks(voice, k, barFeel(score, mb));
          const t0 = barStart + secAt(from);
          const dur = secAt(to) - secAt(from);
          if (ti === displayTrack && vi === 0) beats.push({ t: t0, dur, bar: bi, beat: k, occ });
          if (!play || beat.rest) return;
          for (const n of beat.notes) addNote(track, ti, beat, n, t0, dur);
        });
      });
    }
    time += barDur;
    tick += len;
  }

  function addNote(track, ti, beat, n, t0, dur) {
    const open = (track.tuning[n.string] ?? 40) + (track.capo || 0);
    const last = lastOnString[ti][n.string];
    if (n.tie && last) {
      last.dur = Math.max(last.dur, t0 + dur - last.t);
      // a bend can carry on through tied notes (bend, hold, release): extend the curve
      if (n.bend && n.bend.length) {
        last.bend = last.bend || [];
        for (const p of n.bend) if (last.bend.length < 64) last.bend.push({ t: t0 - last.t + p.t * dur, semis: p.semis });
      }
      if (n.letRing) last.ring = true;
      return;
    }
    let midi = open + n.fret;
    if (n.harmonic === 'natural') midi = open + naturalHarmonicOffset(n.fret);
    else if (n.harmonic && n.harmonic !== 'semi') midi += n.harmonic === 'tap' ? (n.harmonicFret || 12) : 12;
    const dyn = n.dynamic ?? 6;
    let vel = 0.35 + dyn * 0.08;
    if (n.ghost) vel *= 0.55;
    if (n.accent) vel = Math.min(1.15, vel * (n.accent === 'heavy' ? 1.35 : 1.2));
    const ev = {
      t: t0, dur, midi, vel, track: ti, string: n.string,
      kind: n.dead ? 'dead' : n.palmMute ? 'muted' : n.harmonic ? 'harmonic' : 'pluck',
    };
    if (n.staccato) ev.dur = dur * 0.5;
    if (n.palmMute) ev.dur = Math.min(dur, 0.35);
    if (n.letRing || beat.letRing) ev.ring = true;
    if (n.bend) ev.bend = n.bend.map((p) => ({ t: p.t * dur, semis: p.semis })); // seconds from the note start (at speed 1)
    if (n.vibrato || beat.vibrato) ev.vibrato = true;
    if (last && (last.hammer || last.slideLegato)) ev.legato = true;
    if (n.hammer) ev.hammer = true;
    if (n.slide === 'shift' || n.slide === 'legato') {
      ev.slideNext = true;
      ev.slideLegato = n.slide === 'legato';
    } else if (n.slide === 'outDown') ev.slideOut = -5;
    else if (n.slide === 'outUp') ev.slideOut = 5;
    if (n.slideIn) ev.slideIn = n.slideIn === 'below' ? -3 : 3;
    if (last && last.slideNext && !last.slideTarget) last.slideTarget = midi;
    if (n.grace) {
      const g = n.grace;
      const gd = Math.min(0.06, dur / 3);
      notes.push({ t: g.onBeat ? t0 : Math.max(0, t0 - gd), dur: gd, midi: open + g.fret, vel: vel * 0.8,
        track: ti, string: n.string, kind: g.dead ? 'dead' : 'pluck', slideTarget: g.slide ? midi : undefined });
      if (g.hammer || g.slide) ev.legato = true;
      if (g.onBeat) ev.t = t0 + gd;
    }
    if (beat.brush) {
      // spread the strum over ~30 ms, low to high for a down strum
      const order = beat.brush === 'down' ? track.tuning.length - 1 - n.string : n.string;
      ev.t += order * 0.006;
    }
    if (beat.tremolo) {
      const step = (4 / beat.tremolo) * (dur / (beat.ticks / QUARTER)) ;
      for (let tt = t0; tt < t0 + dur - step / 2; tt += step) notes.push({ ...ev, t: tt, dur: step });
      lastOnString[ti][n.string] = notes[notes.length - 1];
      return;
    }
    notes.push(ev);
    lastOnString[ti][n.string] = ev;
  }

  // Let-ring notes sustain until the next note on the same string.
  const nextOnString = new Map();
  for (let i = notes.length - 1; i >= 0; i--) {
    const n = notes[i];
    const key = n.track * 16 + n.string;
    const next = nextOnString.get(key);
    if (n.ring) n.dur = Math.min(MAX_RING, (next ? next.t : n.t + MAX_RING) - n.t);
    if (next && n.t + n.dur > next.t) n.dur = Math.max(0.01, next.t - n.t); // one sound per string
    nextOnString.set(key, n);
  }
  notes.sort((a, b) => a.t - b.t);

  return { duration: time, bars, notes, beats, clicks, order };
}

// Time range [start, end) in seconds for a loop from bar a to bar b (1-based, inclusive).
export function loopRange(tl, fromBar, toBar) {
  if (!fromBar && !toBar) return [0, tl.duration];
  const a = Math.max(1, fromBar || 1) - 1;
  const b = Math.max(a + 1, toBar || fromBar) - 1;
  const first = tl.bars.find((x) => x.index === a);
  if (!first) return [0, tl.duration];
  const last = tl.bars.find((x) => x.index === b && x.start >= first.start) || first;
  return [first.start, last.end];
}

// Count-in clicks: one bar of the first bar's meter at its tempo.
export function countInClicks(score, tl, startTime = 0) {
  const bar = tl.bars.find((b) => b.end > startTime + 1e-6) || tl.bars[0];
  if (!bar) return { clicks: [], dur: 0 };
  const mb = score.masterBars[bar.index];
  const compound = mb.den === 8 && mb.num % 3 === 0 && mb.num > 3;
  const count = compound ? mb.num / 3 : mb.num;
  const sec = (barTicks(mb) * 60) / (bar.bpm * QUARTER);
  const clicks = [];
  for (let k = 0; k < count; k++) clicks.push({ t: (k * sec) / count, down: k === 0 });
  return { clicks, dur: sec };
}

// Swing ("triplet feel") of a bar: GP3/4 store one setting for the song, GP5 a byte per bar
// (1 = 8ths, 2 = 16ths), GP6+ a name.
export function barFeel(score, mb) {
  if (mb.feel) return mb.feel;
  if (mb.tripletFeel === 1) return 'Triplet8th';
  if (mb.tripletFeel === 2) return 'Triplet16th';
  return mb.tripletFeel === undefined && score.tripletFeel ? 'Triplet8th' : '';
}

// Guitar Pro plays a pair of 8ths (or 16ths) inside one beat unevenly, e.g. as a triplet
// quarter + eighth. Only beats forming such a pair move; the rest stay straight.
// Same rules as feelTicks() in plugin/src/core/timeline.cpp.
export function feelTicks(voice, k, feel) {
  const b = voice[k];
  const s = b.start;
  const e = b.start + b.ticks;
  const tup = (x) => !!(x.tuplet && x.tuplet[0]);
  if (!feel || tup(b)) return [s, e];
  const sixteenth = feel.endsWith('16th');
  const triplet = feel.startsWith('Triplet');
  const unit = sixteenth ? QUARTER / 4 : QUARTER / 2;
  const group = unit * 2;
  const first = triplet ? Math.trunc((group * 2) / 3) : feel.startsWith('Dotted') ? Math.trunc((group * 3) / 4) : Math.trunc(group / 4);
  const off = s % group;
  const base = s - off;
  const plain = (j, start, ticks) => j >= 0 && j < voice.length && voice[j].start === start && voice[j].ticks === ticks && !tup(voice[j]);
  for (const sp of triplet ? [unit, (unit * 3) / 2] : [unit]) {
    if (off === 0 && b.ticks === sp && plain(k + 1, s + sp, group - sp)) return [s, s + first];
    if (off === sp && b.ticks === group - sp && plain(k - 1, base, sp)) return [base + first, base + group];
  }
  return [s, e];
}
