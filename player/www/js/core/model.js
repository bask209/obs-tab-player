// SPDX-License-Identifier: Apache-2.0
// Plain-object score model shared by all importers, the renderer and the player.
//
// score:  { format, title, subTitle, artist, album, words, music, copyright, tab, instructions,
//           notices, tempo, tempoLabel, masterBars[], tracks[] }
// master: { num, den, repeatStart, repeatPlays, alternate (bitmask), marker, doubleBar, tempo? }
// track:  { name, tuning[] (MIDI, index 0 = highest string), capo, percussion, bars[] }
// bar:    { voices: [[beat, ...], ...] }
// beat:   { value, dots, tuplet, ticks, start, rest, notes[], chord, text, pickStroke, ... }
// note:   { string (0 = highest), fret, tie, dead, ghost, hammer, slide, bend[], harmonic, ... }

export const QUARTER = 960;

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
export function naturalHarmonicOffset(fret) {
  const table = { 12: 12, 7: 19, 19: 19, 5: 24, 24: 24, 4: 28, 9: 28, 16: 28, 3: 31, 2.4: 34, 2.7: 31 };
  return table[fret] ?? fret + 12;
}
