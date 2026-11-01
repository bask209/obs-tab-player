// SPDX-License-Identifier: Apache-2.0
// Overlay (OBS Browser Source): renders the tab, plays it and follows the server state.

import { loadScore } from './core/load.js';
import { buildTimeline } from './core/timeline.js';
import { Synth } from './core/synth.js';
import { Player } from './core/player.js';
import { TabView } from './app/tabview.js';
import { DEFAULTS, connect, describeFile, fileTitle, parseNotes, scoreRange } from './app/shared.js';

const params = new URLSearchParams(location.search);
const muted = params.get('muted') === '1';
const $ = (id) => document.getElementById(id);
const frame = $('frame');

const synth = new Synth();
const player = new Player(synth);
const view = new TabView($('sheet'));
window.tabPlayer = { synth, player, view }; // for the OBS browser-source dev tools

let state = { ...DEFAULTS };
let score = null;
let tl = null;
let loadedId = null;
let loadingId = null;
let error = '';
let raf = 0;
const diag = { frames: 0, visibility: document.visibilityState };

// ------------------------------------------------------------ state

function onState(next) {
  const prev = state;
  state = { ...DEFAULTS, ...next };
  for (const c of [...frame.classList]) if (/^(theme|font|panel)-/.test(c)) frame.classList.remove(c);
  frame.classList.add('theme-' + state.theme, 'font-' + state.font, 'panel-' + state.panel);
  frame.style.setProperty('--zoom', state.zoom);
  view.setOptions(state);

  if (state.fileId && state.fileId !== loadingId) load(state.fileId);
  else if (score && prev.track !== state.track) rebuild();

  synth.setVolume(muted ? 0 : Number(state.volume));
  synth.setClickVolume(muted ? 0 : Number(state.clickVolume));
  synth.setTone(state.tone);
  player.countIn = !!state.countIn;
  player.metronome = !!state.metronome;
  player.setSpeed(Number(state.speed) || 1);
  player.setLoop(!!state.loop, Number(state.loopFrom) || 0, Number(state.loopTo) || 0);
  header();
  report(true);
}

async function load(id) {
  loadingId = id;
  error = '';
  player.stop();
  message('');
  try {
    const res = await fetch('/api/file?id=' + encodeURIComponent(id));
    if (!res.ok) throw new Error(res.status === 404 ? 'file not found' : res.statusText);
    const bytes = new Uint8Array(await res.arrayBuffer());
    if (loadingId !== id) return;
    score = await loadScore(bytes);
    loadedId = id;
    rebuild();
  } catch (e) {
    score = null;
    tl = null;
    error = `Cannot open ${describeFile(id).name}: ${e.message}`;
    message(error);
    view.setScore(null, null);
  }
  header();
  report(true);
}

function rebuild() {
  const track = Math.min(state.track || 0, score.tracks.length - 1);
  tl = buildTimeline(score, { displayTrack: Math.max(0, track) });
  player.load(score, tl);
  player.setLoop(!!state.loop, Number(state.loopFrom) || 0, Number(state.loopTo) || 0);
  view.setScore(score, tl);
}

// ------------------------------------------------------------ header

function header() {
  const f = loadedId ? describeFile(loadedId) : null;
  $('title').textContent = f ? fileTitle(f) : (state.fileId ? 'Loading…' : 'Pick a tab in the Tab Player dock');
  $('subtitle').textContent = score ? [score.subTitle, score.artist].filter(Boolean).join(' · ') : '';
  const notes = $('notes');
  notes.textContent = '';
  if (score && state.showNotes) {
    const n = parseNotes(score.notices);
    const add = (label, text, cls) => {
      if (!text) return;
      const p = document.createElement('p');
      if (cls) p.className = cls;
      if (label) { const b = document.createElement('b'); b.textContent = label + ' '; p.append(b); }
      p.append(text);
      notes.append(p);
    };
    if (state.showAllNotes) for (const l of n.lines) { if (!/^Source:/i.test(l)) add('', l, 'x'); }
    else {
      add('', n.description || (!n.tip ? n.lines.slice(0, 2).join(' ') : ''));
      add('Tip:', n.tip);
      for (const l of n.extra) add('', l, 'x');
    }
  }
  const base = tl?.bars[0]?.bpm || score?.tempo || 0;
  const speed = Number(state.speed) || 1;
  $('bpm').textContent = base ? `${Math.round(base * speed)} BPM · ${speed.toFixed(2)}×` : '';
  const range = score ? scoreRange(score) : null;
  const flags = [];
  if (range) flags.push(`${range[0]}–${range[1]}`);
  if (state.loop) flags.push(state.loopFrom ? `loop ${state.loopFrom}–${state.loopTo || state.loopFrom}` : 'loop');
  if (state.metronome) flags.push('click');
  $('flags').textContent = flags.join(' · ');
}

function message(t) { $('message').textContent = t; }

// ------------------------------------------------------------ playback + cursor

function frameLoop() {
  raf = 0;
  diag.frames++;
  const now = synth.ctx ? synth.ctx.currentTime : 0;
  const t = player.position(now - (Number(state.avOffset) || 0) / 1000);
  view.place(t);
  const ci = player.countInBeat(now);
  $('countIn').textContent = ci ? String(ci) : '';
  report(false);
  if (player.playing) raf = requestAnimationFrame(frameLoop);
}

player.onstate = () => {
  if (player.playing && !raf) raf = requestAnimationFrame(frameLoop);
  if (!player.playing) { $('countIn').textContent = ''; view.place(player.position()); }
  report(true);
};

async function transport(type, value) {
  if (!tl) return;
  switch (type) {
    case 'play': await player.play(); break;
    case 'pause': player.pause(); break;
    case 'playPause': if (player.playing) player.pause(); else await player.play(); break;
    case 'stop': player.stop(); break;
    case 'restart': player.stop(); await player.play(); break;
    case 'seek': player.seek(Number(value) || 0); view.place(player.position()); break;
  }
  if (player.playing && synth.ctx.state !== 'running') message('Audio is blocked by the browser. Click this page once to enable sound.');
}

// ------------------------------------------------------------ status to the dock

let lastReport = 0;
function report(force) {
  if (muted) return;
  const now = performance.now();
  if (!force && now - lastReport < 400) return;
  lastReport = now;
  const t = player.position();
  let bar = 0;
  if (tl) { for (const b of tl.bars) { if (b.start <= t + 1e-6) bar = b.index + 1; else break; } }
  const body = {
    fileId: loadedId,
    playing: player.playing,
    time: t,
    duration: tl ? tl.duration : 0,
    bar,
    bars: score ? score.masterBars.length : 0,
    baseTempo: tl?.bars[0]?.bpm || score?.tempo || 0,
    range: score ? scoreRange(score) : null,
    tracks: score ? score.tracks.map((x) => x.name || 'Track') : [],
    format: score?.format || '',
    error,
    diag: { ...diag, cursorX: view.cursorX, visibility: document.visibilityState, ctx: synth.ctx ? [synth.ctx.state, +synth.ctx.currentTime.toFixed(2)] : null, raf: !!raf },
  };
  fetch('/api/status', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) }).catch(() => {});
}

document.addEventListener('pointerdown', () => { synth.resume(); message(error); });
document.addEventListener('keydown', (e) => { if (e.code === 'Space') { e.preventDefault(); transport('playPause'); } });

connect(muted ? 'preview' : 'overlay', {
  state: onState,
  transport: (d) => transport(d.type, d.value),
  open: () => report(true),
});
