// SPDX-License-Identifier: Apache-2.0
// Original practice exercises shipped as demo content (Apache-2.0, free to use).
// Strings: 0 = high E ... 5 = low E. Standard tuning.

export const STANDARD = [64, 59, 55, 50, 45, 40];

const n = (string, fret, extra = {}) => ({ string, fret, ...extra });
const beat = (value, notes, extra = {}) => ({ value, notes: Array.isArray(notes) ? notes : [notes], ...extra });
const rest = (value) => ({ value, rest: true, notes: [] });
const bar = (...beats) => ({ voices: [beats] });
const song = (title, tempo, range, description, tip, masterBars, bars, extra = {}) => ({
  title, subTitle: extra.subTitle || '', artist: 'OBS Tab Player demo', music: 'OBS Tab Player contributors', tempo,
  notices: [`Practice tempo: ${range[0]}-${range[1]} BPM.`, `Description: ${description}`, `Tip: ${tip}`],
  masterBars, tracks: [{ name: extra.track || 'Guitar', tuning: STANDARD, capo: 0, bars }],
});
const meter = (count, extra = []) => Array.from({ length: count }, (_, i) => ({ num: 4, den: 4, ...(extra[i] || {}) }));

// 01: alternate picking, A minor pentatonic in eighth notes
function alternatePicking() {
  const up = [[5, 5], [5, 8], [4, 5], [4, 7], [3, 5], [3, 7], [2, 5], [2, 7]];
  const down = [[1, 5], [1, 8], [0, 5], [0, 8], [0, 5], [1, 8], [1, 5], [2, 7]];
  const pick = (k) => (k % 2 ? 'up' : 'down');
  const b1 = bar(...up.map(([s, f], k) => beat(8, n(s, f), { pickStroke: pick(k) })));
  const b2 = bar(...down.map(([s, f], k) => beat(8, n(s, f), { pickStroke: pick(k) })));
  const b3 = bar(...[...up].reverse().map(([s, f], k) => beat(8, n(s, f), { pickStroke: pick(k) })));
  const b4 = bar(beat(2, n(5, 5), { chord: 'Am' }), rest(2));
  return song('Demo 01 - Alternate Picking', 90, [60, 140],
    'A minor pentatonic in the fifth position, up and back down in steady eighth notes.',
    'Keep the picking hand moving down-up even through the string changes. Start slow and stay relaxed.',
    meter(4, [{ repeatStart: true }, {}, {}, { repeatPlays: 2 }]), [b1, b2, b3, b4]);
}

// 02: legato triplets with hammer-ons and pull-offs
function legato() {
  const trip = { tuplet: [3, 2] };
  const ham = (s, a, b, c) => [beat(8, n(s, a, { hammer: true }), trip), beat(8, n(s, b, { hammer: true }), trip), beat(8, n(s, c), trip)];
  const pull = (s, a, b, c) => [beat(8, n(s, a, { hammer: true }), trip), beat(8, n(s, b, { hammer: true }), trip), beat(8, n(s, c), trip)];
  return song('Demo 02 - Legato', 70, [50, 120],
    'Hammer-on and pull-off triplets on the G and B strings. Pick only the first note of each group.',
    'Hammer from the fingertip with enough force for every note to ring as loud as the picked one.',
    meter(2, [{ repeatStart: true }, { repeatPlays: 2 }]),
    [bar(...ham(2, 5, 7, 8), ...pull(2, 8, 7, 5), ...ham(1, 5, 6, 8), ...pull(1, 8, 6, 5)),
      bar(...ham(2, 7, 9, 10), ...pull(2, 10, 9, 7), beat(4, n(1, 8, { vibrato: true })), rest(4))]);
}

// 03: bends, releases and vibrato
function bends() {
  const full = [{ t: 0, semis: 0 }, { t: 0.4, semis: 2 }, { t: 1, semis: 2 }];
  const release = [{ t: 0, semis: 2 }, { t: 0.6, semis: 0 }, { t: 1, semis: 0 }];
  const half = [{ t: 0, semis: 0 }, { t: 0.4, semis: 1 }, { t: 1, semis: 1 }];
  return song('Demo 03 - Bends and Vibrato', 72, [50, 100],
    'Whole-step and half-step bends on the B and G strings, with a release and a vibrato finish.',
    'Bend with three fingers on the string and check the pitch against the target note.',
    meter(2),
    [bar(beat(4, n(1, 15, { bend: full })), beat(4, n(1, 15, { bend: release })), beat(4, n(0, 12)), beat(4, n(1, 15))),
      bar(beat(4, n(2, 14, { bend: half })), beat(4, n(2, 12)), beat(2, n(3, 14, { vibrato: true })))]);
}

// 04: palm-muted rhythm with accents and alternate endings
function rhythm() {
  const pm = (fret = 0) => beat(8, [n(5, fret, { palmMute: true }), n(4, fret + 2, { palmMute: true })]);
  const acc = (fret) => beat(8, [n(5, fret, { accent: true }), n(4, fret + 2, { accent: true })], { pickStroke: 'down' });
  return song('Demo 04 - Palm-Muted Rhythm', 110, [80, 160],
    'Chugging E5 power chords with accented hits. The second time through, the ending changes.',
    'Rest the edge of your picking hand on the strings near the bridge, lift it slightly for the accents.',
    [{ num: 4, den: 4, repeatStart: true }, { num: 4, den: 4, alternate: 1, repeatPlays: 2 }, { num: 4, den: 4, alternate: 2 }],
    [bar(pm(), pm(), acc(0), pm(), pm(), acc(3), pm(), acc(5)),
      bar(pm(), pm(), acc(3), pm(), acc(5), pm(), acc(7), acc(5)),
      bar(beat(2, [n(5, 0), n(4, 2), n(3, 2)], { chord: 'E5' }), rest(2))]);
}

// 05: let-ring arpeggios over a chord progression
function arpeggios() {
  const shape = { Am: [[4, 0], [3, 2], [2, 2], [1, 1]], F: [[3, 3], [2, 2], [1, 1], [0, 1]], C: [[4, 3], [3, 2], [2, 0], [1, 1]], G: [[5, 3], [3, 0], [2, 0], [0, 3]] };
  const arp = (name) => {
    const s = shape[name];
    const order = [0, 1, 2, 3, 2, 1, 2, 3];
    return order.map((k, i) => beat(8, n(s[k][0], s[k][1], { letRing: true }), i === 0 ? { chord: name } : {}));
  };
  return song('Demo 05 - Arpeggios', 84, [60, 132],
    'Let-ring arpeggios through Am, F, C and G. Each chord is picked one string at a time.',
    'Hold the whole chord shape down for the full bar so every note keeps ringing.',
    meter(2, [{ marker: 'Progression' }]),
    [bar(...arp('Am').slice(0, 4), ...arp('F').slice(0, 4)), bar(...arp('C').slice(0, 4), ...arp('G').slice(0, 4))]);
}

export const DEMO_SONGS = [alternatePicking(), legato(), bends(), rhythm(), arpeggios()];
