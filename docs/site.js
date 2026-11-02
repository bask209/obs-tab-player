// SPDX-License-Identifier: Apache-2.0
// The playable tab on the website: the plugin's own engine, compiled to WebAssembly, running in
// the page. Files you open are read in the browser and never leave your computer.
import { openCore } from './app/core.mjs';
import { TabView } from './tabview.js';
import { Player } from './audio.js';

const $ = (id) => document.getElementById(id);
const core = await openCore();
const player = new Player(core);
const view = new TabView($('demoView'), core);
view.setOptions({ zoom: 1.15, layout: 'horizontal' });
let tl = null;

const demos = await fetch('demo/index.json').then((r) => r.json());
for (const f of demos) $('demoSong').append(new Option(f.replace(/^Demo \d+ - |\.gp5$/g, ''), f));

function show() {
  tl = core.timeline();
  player.reset();
  view.show(tl);
  // the box grows with the effect lanes, so nothing above the strings is cut off
  $('demoView').style.height = `${Math.max(220, Math.ceil(view.geo?.height || 0))}px`;
  showBpm();
}

function open(bytes, name) {
  const info = core.load(bytes, player.sampleRate);
  core.setMetronome($('demoClick').checked);
  const score = core.score();
  const tracks = $('demoTrack');
  tracks.replaceChildren(...score.tracks.map((t, i) => new Option(t.name || `Track ${i + 1}`, i)));
  tracks.value = info.track;
  $('trackField').hidden = score.tracks.length < 2;
  $('demoMessage').textContent = name ? `${name}: ${score.title || 'untitled'}` : '';
  show();
}

async function openDemo(file) {
  $('demoMessage').textContent = '';
  open(new Uint8Array(await (await fetch('demo/' + encodeURIComponent(file))).arrayBuffer()));
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
  const ci = player.countInBeat();
  $('demoCount').textContent = ci || '';
  const wait = ci ? (view.place(player.position()), 0.05) : view.follow(player.position(), player.speed);
  timer = setTimeout(follow, Math.max(8, wait * 1000 + 3));
}
player.onstate = () => {
  $('demoPlay').textContent = player.playing ? 'Pause' : 'Play';
  if (player.playing) follow();
  else { clearTimeout(timer); $('demoCount').textContent = ''; view.place(player.position()); }
};

let own = null;
$('demoSong').addEventListener('change', (e) => (e.target.value ? openDemo(e.target.value) : own && open(own.bytes, own.name)));
$('demoTrack').addEventListener('change', (e) => { core.selectTrack(Number(e.target.value)); show(); });
$('demoFile').addEventListener('change', async (e) => {
  const file = e.target.files[0];
  e.target.value = '';
  if (!file) return;
  try {
    open(new Uint8Array(await file.arrayBuffer()), file.name);
    // your file is listed first in the menu, so you can go back to it after trying an exercise
    own = { name: file.name, bytes: new Uint8Array(await file.arrayBuffer()) };
    const option = $('demoSong').querySelector('option[value=""]') || $('demoSong').insertBefore(new Option('', ''), $('demoSong').firstChild);
    option.textContent = file.name;
    $('demoSong').value = '';
  } catch (err) {
    $('demoMessage').textContent = `${file.name} could not be opened: ${err.message}`;
  }
});
$('demoPlay').addEventListener('click', () => (player.playing ? player.pause() : player.play()));
$('demoSpeed').addEventListener('input', (e) => { player.setSpeed(Number(e.target.value)); showBpm(); follow(); });
$('demoClick').addEventListener('change', (e) => { player.metronome = e.target.checked; });
await openDemo(demos[0]);
