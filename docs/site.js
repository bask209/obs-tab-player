// SPDX-License-Identifier: Apache-2.0
// Live demo on the website: the OBS overlay engine running in the page, no server.
import { loadScore } from './app/js/core/load.js';
import { buildTimeline } from './app/js/core/timeline.js';
import { Synth } from './app/js/core/synth.js';
import { Player } from './app/js/core/player.js';
import { TabView } from './app/js/app/tabview.js';

const $ = (id) => document.getElementById(id);
const synth = new Synth();
const player = new Player(synth);
const view = new TabView($('demoView'));
const opts = { theme: 'dark', zoom: 1.1, layout: 'page', cursor: 'both', highlightNotes: true, rhythm: true, labels: 'tab', barNumbers: true };
view.setOptions(opts);
let tl = null;
let raf = 0;

const files = await fetch('demo/index.json').then((r) => r.json());
for (const f of files) $('demoSong').append(new Option(f.replace(/^Demo \d+ - |\.gp5$/g, ''), f));

async function load(file) {
  player.stop();
  const score = await loadScore(await (await fetch('demo/' + encodeURIComponent(file))).arrayBuffer());
  tl = buildTimeline(score);
  player.load(score, tl);
  view.setScore(score, tl);
  showBpm();
}

function showBpm() {
  const base = tl?.bars[0]?.bpm || 0;
  $('demoBpm').textContent = `${Math.round(base * player.speed)} BPM`;
}

function frame() {
  raf = 0;
  view.place(player.position());
  const ci = player.countInBeat();
  $('demoCount').textContent = ci ? ci : '';
  if (player.playing) raf = requestAnimationFrame(frame);
}
player.onstate = () => {
  $('demoPlay').textContent = player.playing ? '❚❚ Pause' : '▶ Play';
  if (player.playing && !raf) raf = requestAnimationFrame(frame);
  if (!player.playing) $('demoCount').textContent = '';
};

$('demoSong').addEventListener('change', (e) => load(e.target.value));
$('demoPlay').addEventListener('click', () => (player.playing ? player.pause() : player.play()));
$('demoSpeed').addEventListener('input', (e) => { player.setSpeed(Number(e.target.value)); showBpm(); });
$('demoTheme').addEventListener('change', (e) => {
  opts.theme = e.target.value;
  view.setOptions(opts);
  $('demoView').style.background = e.target.value === 'clear' ? 'transparent' : '';
});
$('demoColors').addEventListener('change', (e) => { opts.colorStrings = e.target.checked; view.setOptions(opts); });
$('demoClick').addEventListener('change', (e) => { player.metronome = e.target.checked; });
player.loop = true;
player.countIn = true;
await load(files[0]);
