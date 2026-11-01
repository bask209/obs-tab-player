// SPDX-License-Identifier: Apache-2.0
// Writes simple Guitar Pro 3/4/5 files (.gp3/.gp4/.gp5). Used to build the demo
// exercises and to round-trip test the reader across all three binary versions.
//
// writeGp(song, version) where version is '3.00', '4.06', '5.00' or '5.10' and song is:
// { title, subTitle, artist, music, notices: [..], tempo, masterBars: [{ num, den, repeatStart,
//   repeatPlays, alternate (ending number), marker }], tracks: [{ name, tuning (high to low),
//   capo, bars: [{ voices: [[beat]] }] }] }
// beat: { value, dots, tuplet: [n, d], rest, chord, text, pickStroke, vibrato,
//   notes: [{ string, fret, tie, dead, ghost, accent, dynamic, hammer, slide, letRing,
//   palmMute, staccato, vibrato, harmonic, bend: [{ t, semis }] }] }

const enc = (s) => Uint8Array.from(String(s || ''), (c) => Math.min(255, c.charCodeAt(0)));

class Writer {
  constructor() { this.buf = new Uint8Array(1 << 16); this.len = 0; this.dv = new DataView(this.buf.buffer); }
  grow(n) {
    if (this.len + n <= this.buf.length) return;
    const b = new Uint8Array(Math.max(this.buf.length * 2, this.len + n));
    b.set(this.buf);
    this.buf = b;
    this.dv = new DataView(b.buffer);
  }
  u8(v) { this.grow(1); this.buf[this.len++] = v & 0xff; }
  i8(v) { this.u8(v < 0 ? 256 + v : v); }
  i16(v) { this.grow(2); this.dv.setInt16(this.len, v, true); this.len += 2; }
  i32(v) { this.grow(4); this.dv.setInt32(this.len, v, true); this.len += 4; }
  bytes(b) { this.grow(b.length); this.buf.set(b, this.len); this.len += b.length; }
  zeros(n) { this.grow(n); this.len += n; }
  byteSizeString(s, size) { const b = enc(s).subarray(0, size); this.u8(b.length); this.bytes(b); this.zeros(size - b.length); }
  intByteSizeString(s) { const b = enc(s).subarray(0, 255); this.i32(b.length + 1); this.u8(b.length); this.bytes(b); }
  intSizeString(s) { const b = enc(s); this.i32(b.length); this.bytes(b); }
  result() { return this.buf.slice(0, this.len); }
}

export function writeGp(song, version = '5.10') {
  const ver = Math.round(Number(version) * 100);
  const w = new Writer();
  w.byteSizeString(`FICHIER GUITAR PRO v${version}`, 30);
  w.intByteSizeString(song.title);
  w.intByteSizeString(song.subTitle);
  w.intByteSizeString(song.artist);
  w.intByteSizeString(song.album);
  w.intByteSizeString(song.words);
  if (ver >= 500) w.intByteSizeString(song.music);
  w.intByteSizeString(song.copyright);
  w.intByteSizeString(song.tab);
  w.intByteSizeString(song.instructions);
  const notices = song.notices || [];
  w.i32(notices.length);
  for (const n of notices) w.intByteSizeString(n);
  if (ver < 500) w.u8(0); // triplet feel
  if (ver >= 400) { w.i32(1); for (let i = 0; i < 5; i++) { w.i32(1); w.intSizeString(''); } }
  if (ver >= 510) w.zeros(19);
  if (ver >= 500) {
    w.i32(210); w.i32(297); w.i32(10); w.i32(10); w.i32(15); w.i32(10); w.i32(100); w.i16(0xff);
    for (const t of ['%TITLE%', '%SUBTITLE%', '%ARTIST%', '%ALBUM%', 'Words by %WORDS%', 'Music by %MUSIC%', 'Words & Music by %WORDSMUSIC%', 'Copyright %COPYRIGHT%', 'All Rights Reserved', 'Page %N%/%P%']) w.intByteSizeString(t);
    w.intByteSizeString(song.tempoLabel || 'Moderate');
  }
  w.i32(song.tempo || 120);
  if (ver >= 510) w.u8(0);
  w.i32(0); // key
  if (ver >= 400) w.u8(0); // octave
  for (let i = 0; i < 64; i++) { w.i32(i === 9 ? 0 : 25); w.u8(104); w.u8(64); w.zeros(6); }
  if (ver >= 500) { for (let i = 0; i < 19; i++) w.i16(-1); w.i32(0); }
  const bars = song.masterBars;
  w.i32(bars.length);
  w.i32(song.tracks.length);

  let prev = null;
  for (const mb of bars) {
    let flags = 0;
    if (!prev || prev.num !== mb.num) flags |= 0x01;
    if (!prev || prev.den !== mb.den) flags |= 0x02;
    if (mb.repeatStart) flags |= 0x04;
    if (mb.repeatPlays) flags |= 0x08;
    if (mb.alternate) flags |= 0x10;
    if (mb.marker) flags |= 0x20;
    w.u8(flags);
    if (flags & 0x01) w.u8(mb.num);
    if (flags & 0x02) w.u8(mb.den);
    if (flags & 0x08) w.u8(ver >= 500 ? mb.repeatPlays : mb.repeatPlays - 1);
    if (flags & 0x10 && ver < 500) w.u8(mb.alternate);
    if (flags & 0x20) { w.intByteSizeString(mb.marker); w.bytes([255, 0, 0, 0]); }
    if (ver >= 500) {
      if (flags & 0x03) w.bytes([2, 2, 2, 2]);
      w.u8(flags & 0x10 ? 1 << (mb.alternate - 1) : 0);
      w.u8(0); // triplet feel
      w.u8(0);
    }
    prev = mb;
  }

  song.tracks.forEach((t, ti) => {
    w.u8(0x08);
    w.byteSizeString(t.name, 40);
    w.i32(t.tuning.length);
    for (let i = 0; i < 7; i++) w.i32(t.tuning[i] ?? 0);
    w.i32(1); w.i32(ti + 1); w.i32(ti + 1); w.i32(24); w.i32(t.capo || 0);
    w.bytes([0, 0, 255, 0]);
    if (ver >= 500) { w.u8(0x03); w.u8(0); w.zeros(43); }
    if (ver >= 510) { w.zeros(4); w.intByteSizeString(''); w.intByteSizeString(''); }
  });

  for (let b = 0; b < bars.length; b++) {
    for (const t of song.tracks) {
      const bar = t.bars[b] || { voices: [[{ value: 1, rest: true, notes: [] }]] };
      if (ver >= 500) w.u8(0);
      const voices = ver >= 500 ? 2 : 1;
      for (let v = 0; v < voices; v++) {
        const beats = bar.voices[v] || [];
        w.i32(beats.length);
        for (const beat of beats) writeBeat(w, ver, beat, t);
      }
    }
  }
  return w.result();
}

function writeBeat(w, ver, beat, track) {
  const notes = beat.rest ? [] : beat.notes || [];
  const fx = (ver < 400 ? beat.vibrato : beat.vibrato || beat.pickStroke);
  let flags = 0;
  if (beat.dots) flags |= 0x01;
  if (beat.chord) flags |= 0x02;
  if (beat.text) flags |= 0x04;
  if (fx) flags |= 0x08;
  if (beat.tuplet) flags |= 0x20;
  if (!notes.length) flags |= 0x40;
  w.u8(flags);
  if (flags & 0x40) w.u8(0x02);
  w.i8(Math.round(Math.log2(beat.value || 4)) - 2);
  if (flags & 0x20) w.i32(beat.tuplet[0]);
  if (flags & 0x02) {
    if (ver >= 500) {
      w.u8(1); w.zeros(16); w.byteSizeString(beat.chord, 21); w.zeros(4); w.i32(0);
      for (let i = 0; i < 7; i++) w.i32(-1);
      w.zeros(32);
    } else if (ver >= 400) {
      w.u8(1); w.zeros(16); w.byteSizeString(beat.chord, 21); w.zeros(4); w.i32(0);
      for (let i = 0; i < 7; i++) w.i32(-1);
      w.zeros(32);
    } else {
      w.u8(0); w.intByteSizeString(beat.chord); w.i32(0);
    }
  }
  if (flags & 0x04) w.intByteSizeString(beat.text);
  if (flags & 0x08) {
    w.u8(beat.vibrato ? (ver < 400 ? 0x01 : 0x02) : 0);
    if (ver >= 400) {
      w.u8(beat.pickStroke ? 0x02 : 0);
      if (beat.pickStroke) w.i8(beat.pickStroke === 'up' ? 1 : 2);
    }
  }
  let stringFlags = 0;
  for (const n of notes) stringFlags |= 1 << (6 - n.string);
  w.u8(stringFlags);
  for (const n of [...notes].sort((a, b) => a.string - b.string)) writeNote(w, ver, n);
  if (ver >= 500) w.i16(0);
}

function writeNote(w, ver, n) {
  const fx2 = ver >= 400 && (n.staccato || n.palmMute || (n.slide && ver >= 400) || n.harmonic || n.vibrato);
  const fx = n.bend || n.hammer || n.letRing || fx2 || (ver < 400 && n.slide);
  let flags = 0x20;
  if (n.ghost) flags |= 0x04;
  if (n.dynamic) flags |= 0x10;
  if (n.accent) flags |= 0x40;
  if (fx) flags |= 0x08;
  w.u8(flags);
  w.u8(n.tie ? 2 : n.dead ? 3 : 1);
  if (flags & 0x10) w.i8(n.dynamic);
  w.i8(n.fret);
  if (ver >= 500) w.u8(0);
  if (!(flags & 0x08)) return;
  let f1 = 0;
  if (n.bend) f1 |= 0x01;
  if (n.hammer) f1 |= 0x02;
  if (ver < 400 && n.slide) f1 |= 0x04;
  if (n.letRing) f1 |= 0x08;
  w.u8(f1);
  let f2 = 0;
  if (ver >= 400) {
    if (n.staccato) f2 |= 0x01;
    if (n.palmMute) f2 |= 0x02;
    if (n.slide) f2 |= 0x08;
    if (n.harmonic) f2 |= 0x10;
    if (n.vibrato) f2 |= 0x40;
    w.u8(f2);
  }
  if (n.bend) {
    w.u8(1);
    w.i32(Math.round(Math.max(...n.bend.map((p) => p.semis)) * 50));
    w.i32(n.bend.length);
    for (const p of n.bend) { w.i32(Math.round(p.t * 60)); w.i32(Math.round(p.semis * 50)); w.u8(0); }
  }
  if (f2 & 0x08) w.i8(ver >= 500 ? (n.slide === 'legato' ? 2 : 1) : (n.slide === 'legato' ? 2 : 1));
  if (f2 & 0x10) w.u8(1);
}
