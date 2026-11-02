// SPDX-License-Identifier: Apache-2.0
// Plain-object score model shared by all importers, the renderer and the player.
//
// score:  { format, title, subTitle, artist, album, words, music, copyright, tab, instructions,
//           notices, tempo, tempoLabel, masterBars[], tracks[] }
// master: { num, den, repeatStart, repeatPlays, alternate (bitmask), marker, doubleBar, tempo?,
//           targets, jump, fermatas: [{ ratio, length }] }
// track:  { name, tuning[] (MIDI, index 0 = highest string), capo, percussion, bars[] }
// bar:    { voices: [[beat, ...], ...] }
// beat:   { value, dots, tuplet, ticks, start, rest, notes[], chord, text, pickStroke, ... }
// note:   { string (0 = highest), fret, tie, dead, ghost, hammer, slide, bend[], harmonic, ... }

export const QUARTER = 960;

// Musical directions. master.targets is a bit set of TARGETS, master.jump an index into JUMPS.
export const TARGETS = ['Coda', 'DoubleCoda', 'Segno', 'SegnoSegno', 'Fine'];
export const JUMPS = ['', 'DaCapo', 'DaCapoAlCoda', 'DaCapoAlDoubleCoda', 'DaCapoAlFine', 'DaSegno', 'DaSegnoAlCoda', 'DaSegnoAlDoubleCoda',
  'DaSegnoAlFine', 'DaSegnoSegno', 'DaSegnoSegnoAlCoda', 'DaSegnoSegnoAlDoubleCoda', 'DaSegnoSegnoAlFine', 'DaCoda', 'DaDoubleCoda'];

export function beatTicks(value, dots = 0, tupletNum = 1, tupletDen = 1) {
  let t = (QUARTER * 4) / value;
  if (dots === 1) t *= 1.5;
  else if (dots >= 2) t *= 1.75;
  if (tupletNum > 1 && tupletDen > 0) t = (t * tupletDen) / tupletNum;
  return Math.round(t);
}

export function barTicks(master) {
  return Math.round((QUARTER * 4 * master.num) / master.den);
}

export function newScore(format) {
  return {
    format, title: '', subTitle: '', artist: '', album: '', words: '', music: '', copyright: '',
    tab: '', instructions: '', notices: '', tempo: 120, tempoLabel: '', masterBars: [], tracks: [],
  };
}

export function newBeat() {
  return { value: 4, dots: 0, tuplet: null, ticks: QUARTER, start: 0, rest: false, notes: [] };
}

export function newNote(string, fret) {
  return { string, fret };
}

// Fill in beat.ticks / beat.start for every voice, resolve tied frets and drop empty voices.
export function finishScore(score) {
  for (const track of score.tracks) {
    // Guitar Pro 6-8 store grace notes as beats of their own: fold each into the note it leads to.
    for (const bar of track.bars) {
      bar.voices = bar.voices.map((voice) => {
        if (!voice.some((b) => b.grace)) return voice;
        const kept = [];
        let grace = null;
        for (const b of voice) {
          if (b.grace) { grace = b; continue; }
          if (grace) {
            for (const n of b.notes) for (const gn of grace.notes) {
              if (gn.string !== n.string || n.grace) continue;
              n.grace = { fret: gn.fret };
              if (gn.slide === 'shift' || gn.slide === 'legato') n.grace.slide = true;
              if (gn.hammer) n.grace.hammer = true;
              if (gn.dead) n.grace.dead = true;
              if (grace.grace === 'onBeat') n.grace.onBeat = true;
            }
            grace = null;
          }
          kept.push(b);
        }
        return kept;
      });
    }
    const lastFret = [[], []];
    for (const bar of track.bars) {
      bar.voices.forEach((voice, vi) => {
        const last = lastFret[vi] || (lastFret[vi] = []);
        for (const b of voice) for (const n of b.notes) {
          if (n.tie && last[n.string] != null) n.fret = last[n.string];
          last[n.string] = n.fret;
        }
      });
    }
    track.bars.forEach((bar, i) => {
      bar.voices = bar.voices.filter((v) => v.length);
      for (const voice of bar.voices) {
        let pos = 0;
        for (const b of voice) {
          b.ticks = beatTicks(b.value, b.dots, b.tuplet?.[0], b.tuplet?.[1]);
          b.start = pos;
          pos += b.ticks;
          b.notes.sort((x, y) => x.string - y.string);
        }
      }
      if (!bar.voices.length) {
        // Keep the bar renderable: a whole-bar rest.
        const m = score.masterBars[i];
        const rest = newBeat();
        rest.rest = true;
        rest.ticks = m ? barTicks(m) : QUARTER * 4;
        rest.value = 1;
        rest.fullBar = true;
        bar.voices = [[rest]];
      }
    });
  }
  return score;
}

// Natural harmonic pitch offset (semitones above the open string) for a fret position.
// The string node nearest the touched fret (m/q of the length) sounds partial q.
export function naturalHarmonicOffset(fret) {
  const r = 2 ** (-fret / 12);
  let best = 1; let partial = 0;
  const gcd = (a, b) => (b ? gcd(b, a % b) : a);
  for (let q = 2; q <= 8; q++) {
    for (let m = 1; m < q; m++) {
      const d = Math.abs(r - m / q);
      if (gcd(m, q) === 1 && d < best - 1e-9) { best = d; partial = q; }
    }
  }
  if (best > 0.012) return Math.round(fret) + 12;
  return Math.round(12 * Math.log2(partial));
}
