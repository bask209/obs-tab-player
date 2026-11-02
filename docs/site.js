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
const opts = { theme: 'clear', zoom: 1.15, layout: 'horizontal', colorStrings: true, cursor: 'both', highlightNotes: true, rhythm: true, labels: 'tab', barNumbers: true };
view.setOptions(opts);
let tl = null;

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

// Wake once per beat; the cursor glides between beats on the compositor.
let timer = 0;
function follow() {
  clearTimeout(timer);
  if (!player.playing) return;
  const now = synth.ctx.currentTime;
  const ci = player.countInBeat(now);
  $('demoCount').textContent = ci ? ci : '';
  const wait = ci ? (view.place(player.position(now)), player.countInWait(now)) : view.follow(player.position(now), player.speed);
  timer = setTimeout(follow, Math.max(8, wait * 1000 + 3));
}
player.onstate = () => {
  $('demoPlay').textContent = player.playing ? 'Pause' : 'Play';
  if (player.playing) follow();
  else { clearTimeout(timer); $('demoCount').textContent = ''; view.place(player.position()); }
};

$('demoSong').addEventListener('change', (e) => load(e.target.value));
$('demoPlay').addEventListener('click', () => (player.playing ? player.pause() : player.play()));
$('demoSpeed').addEventListener('input', (e) => { player.setSpeed(Number(e.target.value)); showBpm(); follow(); });
$('demoClick').addEventListener('change', (e) => { player.metronome = e.target.checked; });
player.loop = true;
player.countIn = true;
await load(files[0]);
