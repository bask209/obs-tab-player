// SPDX-License-Identifier: Apache-2.0
// Reader for the binary Guitar Pro 3, 4 and 5 formats (.gp3, .gp4, .gp5).

import { newScore, newBeat, newNote, finishScore } from './model.js';

const latin1 = new TextDecoder('windows-1252');
const utf8 = new TextDecoder('utf-8', { fatal: true });

// Guitar Pro writes Windows-1252, some other tools write UTF-8. Prefer UTF-8 when it is valid.
function decodeText(bytes) {
  let ascii = true;
  for (let i = 0; i < bytes.length; i++) if (bytes[i] > 0x7f) { ascii = false; break; }
  if (!ascii) { try { return utf8.decode(bytes); } catch { /* not UTF-8 */ } }
  return latin1.decode(bytes);
}

class Reader {
  constructor(bytes) {
    this.b = bytes;
    this.v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    this.p = 0;
  }
  need(n) { if (this.p + n > this.b.length) throw new Error('Unexpected end of file'); }
  skip(n) { this.need(n); this.p += n; }
  u8() { this.need(1); return this.b[this.p++]; }
  i8() { this.need(1); return this.v.getInt8(this.p++); }
  bool() { return this.u8() !== 0; }
  i16() { this.need(2); const x = this.v.getInt16(this.p, true); this.p += 2; return x; }
  i32() { this.need(4); const x = this.v.getInt32(this.p, true); this.p += 4; return x; }
  f64be() { this.need(8); const x = this.v.getFloat64(this.p, false); this.p += 8; return x; }
  str(n) { this.need(n); const s = decodeText(this.b.subarray(this.p, this.p + n)); this.p += n; return s; }
  // byte length + fixed size field
  byteSizeString(size) { const len = Math.min(this.u8(), size); const s = this.str(len); this.skip(size - len); return s; }
  // int (size + 1) + byte length + chars
  intByteSizeString() {
    const d = this.i32();
    const len = this.u8();
    const n = d > 0 ? d - 1 : len;
    const s = this.str(Math.min(len, n));
    this.skip(Math.max(0, n - len));
    return s;
  }
  // int length + chars
  intSizeString() { return this.str(Math.max(0, this.i32())); }
}

export function isGp3to5(bytes) {
  return bytes.length > 31 && latin1.decode(bytes.subarray(1, 19)) === 'FICHIER GUITAR PRO';
}

export function readGp3to5(bytes) {
  const r = new Reader(bytes);
  const versionText = r.byteSizeString(30);
  const m = /v(\d+)\.(\d+)/.exec(versionText);
  if (!m) throw new Error('Not a Guitar Pro 3-5 file');
  const ver = Number(m[1]) * 100 + Number(m[2]);
  const score = newScore(ver >= 500 ? 'gp5' : ver >= 400 ? 'gp4' : 'gp3');
  score.version = versionText;

  score.title = r.intByteSizeString();
  score.subTitle = r.intByteSizeString();
  score.artist = r.intByteSizeString();
  score.album = r.intByteSizeString();
  score.words = r.intByteSizeString();
  score.music = ver >= 500 ? r.intByteSizeString() : score.words;
  score.copyright = r.intByteSizeString();
  score.tab = r.intByteSizeString();
  score.instructions = r.intByteSizeString();
  const noticeCount = r.i32();
  const notices = [];
  for (let i = 0; i < noticeCount; i++) notices.push(r.intByteSizeString());
  score.notices = notices.join('\n');

  if (ver < 500) score.tripletFeel = r.bool();
  if (ver >= 400) {
    r.i32(); // lyrics track
    for (let i = 0; i < 5; i++) { r.i32(); r.intSizeString(); }
  }
  if (ver >= 510) r.skip(19); // RSE master effect
  if (ver >= 500) {
    r.skip(30); // page size, margins, proportion, header/footer flags
    for (let i = 0; i < 10; i++) r.intByteSizeString();
    score.tempoLabel = r.intByteSizeString();
  }
  score.tempo = r.i32();
  if (ver >= 510) r.bool(); // hide tempo
  r.i32(); // key signature
  if (ver >= 400) r.u8(); // octave

  const channels = [];
  for (let i = 0; i < 64; i++) {
    channels.push({ program: r.i32(), volume: r.u8(), balance: r.u8() });
    r.skip(6);
  }
  if (ver >= 500) r.skip(38 + 4); // musical directions + master reverb

  const barCount = r.i32();
  const trackCount = r.i32();
  if (barCount < 0 || barCount > 100000 || trackCount < 0 || trackCount > 128) throw new Error('Corrupt header');

  // ---------------------------------------------------------------- master bars
  let prev = { num: 4, den: 4 };
  for (let i = 0; i < barCount; i++) {
    const flags = r.u8();
    const mb = { num: prev.num, den: prev.den, repeatStart: false, repeatPlays: 0, alternate: 0, marker: '', doubleBar: false };
    if (flags & 0x01) mb.num = r.u8();
    if (flags & 0x02) mb.den = r.u8();
    mb.repeatStart = !!(flags & 0x04);
    if (flags & 0x08) mb.repeatPlays = r.u8() + (ver >= 500 ? 0 : 1);
    if (flags & 0x10 && ver < 500) {
      const n = r.u8();
      mb.alternate = n > 0 && n <= 8 ? 1 << (n - 1) : n;
    }
    if (flags & 0x20) { mb.marker = r.intByteSizeString(); r.skip(4); }
    if (flags & 0x40) { r.i8(); r.u8(); }
    if (ver >= 500) {
      if (flags & 0x03) r.skip(4); // beaming groups
      const alt = r.u8();
      if (flags & 0x10) mb.alternate = alt;
      mb.tripletFeel = r.u8();
      r.u8();
    }
    mb.doubleBar = !!(flags & 0x80);
    if (!mb.num || !mb.den) { mb.num = 4; mb.den = 4; }
    score.masterBars.push(mb);
    prev = mb;
  }

  // ---------------------------------------------------------------- tracks
  for (let t = 0; t < trackCount; t++) {
    const flags = r.u8();
    const track = { name: r.byteSizeString(40), percussion: !!(flags & 0x01), tuning: [], capo: 0, bars: [] };
    const stringCount = r.i32();
    for (let i = 0; i < 7; i++) {
      const tune = r.i32();
      if (i < stringCount) track.tuning.push(tune);
    }
    r.i32(); // port
    const channel = r.i32() - 1;
    r.i32(); // effect channel
    r.i32(); // fret count
    track.capo = r.i32();
    r.skip(4); // color
    if (ver >= 500) r.skip(45);
    if (ver >= 510) { r.skip(4); r.intByteSizeString(); r.intByteSizeString(); }
    const ch = channels[channel];
    track.program = ch ? ch.program : 25;
    track.volume = ch ? ch.volume / 127 : 1;
    if (channel === 9) track.percussion = true;
    score.tracks.push(track);
  }

  // ---------------------------------------------------------------- bars
  for (let i = 0; i < barCount; i++) {
    for (const track of score.tracks) {
      const bar = { voices: [] };
      const voiceCount = ver >= 500 ? 2 : 1;
      if (ver >= 500) r.u8();
      for (let v = 0; v < voiceCount; v++) {
        const beats = [];
        const n = r.i32();
        if (n < 0 || n > 10000) throw new Error('Corrupt bar data');
        for (let k = 0; k < n; k++) beats.push(readBeat(r, ver, track, score.masterBars[i]));
        bar.voices.push(beats);
      }
      track.bars.push(bar);
    }
  }
  return finishScore(score);
}

const TUPLET_DEN = { 3: 2, 5: 4, 6: 4, 7: 4, 9: 8, 10: 8, 11: 8, 12: 8, 13: 8 };

function readBeat(r, ver, track, master) {
  const beat = newBeat();
  const flags = r.u8();
  if (flags & 0x01) beat.dots = 1;
  if (flags & 0x40) {
    const status = r.u8();
    beat.rest = true;
    if ((status & 0x02) === 0) beat.empty = true;
  }
  const d = r.i8();
  beat.value = d >= -2 && d <= 4 ? 1 << (d + 2) : 4;
  if (flags & 0x20) {
    const n = r.i32();
    if (TUPLET_DEN[n]) beat.tuplet = [n, TUPLET_DEN[n]];
  }
  if (flags & 0x02) readChord(r, ver, beat);
  if (flags & 0x04) beat.text = r.intByteSizeString();
  let harmonicAll = null;
  if (flags & 0x08) harmonicAll = readBeatEffects(r, ver, beat);
  if (flags & 0x10) readMixTable(r, ver, beat, master);
  const stringFlags = r.u8();
  for (let i = 6; i >= 0; i--) {
    if (stringFlags & (1 << i) && 6 - i < track.tuning.length) {
      const note = readNote(r, ver, 6 - i, beat);
      if (harmonicAll && !note.harmonic) note.harmonic = harmonicAll;
      beat.notes.push(note);
    }
  }
  if (beat.notes.length) beat.rest = false;
  else beat.rest = true;
  if (ver >= 500) {
    const flags2 = r.i16();
    if (flags2 & 0x0800) r.u8();
  }
  return beat;
}

function readChord(r, ver, beat) {
  const chord = { name: '', firstFret: 0, frets: [] };
  if (ver >= 500) {
    r.skip(17);
    chord.name = r.byteSizeString(21);
    r.skip(4);
    chord.firstFret = r.i32();
    for (let i = 0; i < 7; i++) chord.frets.push(r.i32());
    r.skip(32);
  } else if (r.u8() !== 0) {
    if (ver >= 400) {
      r.skip(16);
      chord.name = r.byteSizeString(21);
      r.skip(4);
      chord.firstFret = r.i32();
      for (let i = 0; i < 7; i++) chord.frets.push(r.i32());
      r.skip(32);
    } else {
      r.skip(25);
      chord.name = r.byteSizeString(34);
      chord.firstFret = r.i32();
      for (let i = 0; i < 6; i++) chord.frets.push(r.i32());
      r.skip(36);
    }
  } else {
    const strings = ver >= 406 ? 7 : 6;
    chord.name = r.intByteSizeString();
    chord.firstFret = r.i32();
    if (chord.firstFret > 0) for (let i = 0; i < strings; i++) chord.frets.push(r.i32());
  }
  if (chord.name) beat.chord = chord.name.trim();
}

function readBendPoints(r) {
  r.u8(); // type
  r.i32(); // value
  const n = r.i32();
  const points = [];
  for (let i = 0; i < n; i++) {
    const pos = r.i32();
    const val = r.i32();
    r.u8(); // vibrato
    points.push({ t: Math.max(0, Math.min(1, pos / 60)), semis: val / 50 });
  }
  return points;
}

function readBeatEffects(r, ver, beat) {
  const flags = r.u8();
  const flags2 = ver >= 400 ? r.u8() : 0;
  if (flags & 0x10) beat.fadeIn = true;
  if ((ver < 400 && flags & 0x01) || flags & 0x02) beat.vibrato = true;
  if (flags & 0x20) {
    const kind = r.i8();
    if (kind === 1) beat.tap = true;
    else if (kind === 2) beat.slap = true;
    else if (kind === 3) beat.pop = true;
    if (ver < 400) r.skip(4);
  }
  if (flags2 & 0x04) beat.whammy = readBendPoints(r);
  if (flags & 0x40) {
    let up, down;
    if (ver < 500) { down = r.u8(); up = r.u8(); } else { up = r.u8(); down = r.u8(); }
    if (up > 0) beat.brush = 'up';
    else if (down > 0) beat.brush = 'down';
  }
  if (flags2 & 0x02) {
    const s = r.i8();
    if (s === 1) beat.pickStroke = 'up';
    else if (s === 2) beat.pickStroke = 'down';
  }
  if (ver < 400) {
    if (flags & 0x04) return 'natural';
    if (flags & 0x08) return 'artificial';
  }
  return null;
}

function readMixTable(r, ver, beat, master) {
  r.i8(); // instrument
  if (ver >= 500) r.skip(16);
  const vals = [];
  for (let i = 0; i < 6; i++) vals.push(r.i8()); // volume, balance, chorus, reverb, phaser, tremolo
  if (ver >= 500) r.intByteSizeString();
  const tempo = r.i32();
  for (const v of vals) if (v >= 0) r.u8();
  if (tempo >= 0) {
    r.i8();
    if (ver >= 510) r.u8();
  }
  if (ver >= 400) r.u8();
  if (ver >= 500) r.i8();
  if (ver >= 510) { r.intByteSizeString(); r.intByteSizeString(); }
  if (tempo > 0) beat.tempo = tempo;
}

function readNote(r, ver, string, beat) {
  const note = newNote(string, 0);
  const flags = r.u8();
  if (flags & 0x04) note.ghost = true;
  if (flags & 0x40) note.accent = true;
  if (flags & 0x02) note.accent = 'heavy';
  if (flags & 0x20) {
    const type = r.u8();
    if (type === 2) note.tie = true;
    else if (type === 3) note.dead = true;
  }
  if (flags & 0x01 && ver < 500) r.skip(2);
  if (flags & 0x10) note.dynamic = r.i8();
  if (flags & 0x20) note.fret = r.i8();
  if (flags & 0x80) { note.leftFinger = r.i8(); note.rightFinger = r.i8(); }
  if (ver >= 500) {
    if (flags & 0x01) r.f64be();
    r.u8();
  }
  if (flags & 0x08) readNoteEffects(r, ver, note, beat);
  if (note.fret < 0 || note.fret > 99) note.fret = 0;
  return note;
}

function readNoteEffects(r, ver, note, beat) {
  const flags = r.u8();
  const flags2 = ver >= 400 ? r.u8() : 0;
  if (flags & 0x01) note.bend = readBendPoints(r);
  if (flags & 0x10) {
    const grace = { fret: r.i8(), dynamic: r.i8() };
    const transition = r.i8();
    if (transition === 1) grace.slide = true;
    else if (transition === 3) grace.hammer = true;
    r.u8(); // duration
    if (ver >= 500) {
      const gf = r.u8();
      if (gf & 0x01) grace.dead = true;
      if (gf & 0x02) grace.onBeat = true;
    }
    note.grace = grace;
  }
  if (flags2 & 0x04) {
    const speed = r.u8();
    beat.tremolo = speed === 1 ? 8 : speed === 3 ? 32 : 16;
  }
  if (flags2 & 0x08) {
    const type = r.i8();
    if (ver >= 500) {
      if (type & 1) note.slide = 'shift';
      else if (type & 2) note.slide = 'legato';
      else if (type & 4) note.slide = 'outDown';
      else if (type & 8) note.slide = 'outUp';
      if (type & 16) note.slideIn = 'below';
      else if (type & 32) note.slideIn = 'above';
    } else {
      note.slide = { 1: 'shift', 2: 'legato', 3: 'outDown', 4: 'outUp' }[type];
      note.slideIn = { [-1]: 'below', [-2]: 'above' }[type];
    }
  } else if (ver < 400 && flags & 0x04) {
    note.slide = 'shift';
  }
  if (flags2 & 0x10) {
    const type = r.u8();
    if (ver >= 500) {
      if (type === 1) note.harmonic = 'natural';
      else if (type === 2) { r.skip(3); note.harmonic = 'artificial'; }
      else if (type === 3) { note.harmonic = 'tap'; note.harmonicFret = r.u8(); }
      else if (type === 4) note.harmonic = 'pinch';
      else if (type === 5) note.harmonic = 'semi';
    } else {
      note.harmonic = { 1: 'natural', 3: 'tap', 4: 'pinch', 5: 'semi', 15: 'artificial', 17: 'artificial', 22: 'artificial' }[type];
    }
  }
  if (flags2 & 0x20) { note.trill = r.u8(); r.u8(); }
  if (flags & 0x08) note.letRing = true;
  if (flags & 0x02) note.hammer = true;
  if (flags2 & 0x40) note.vibrato = true;
  if (flags2 & 0x02) note.palmMute = true;
  if (flags2 & 0x01) note.staccato = true;
}
