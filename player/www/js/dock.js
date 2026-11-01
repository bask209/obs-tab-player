// SPDX-License-Identifier: Apache-2.0
// Control dock (OBS Custom Browser Dock, or any browser tab).

import { loadScore } from './core/load.js';
import { DEFAULTS, connect, command, setState, describeFile, fileTitle, parseNotes, scoreRange } from './app/shared.js';

const $ = (id) => document.getElementById(id);
let state = { ...DEFAULTS };
let status = {};
let files = [];
let shown = [];
let cursor = 0;
let meta = loadMeta();

// ------------------------------------------------------------ controls

document.querySelectorAll('[data-cmd]').forEach((b) => b.addEventListener('click', () => command(b.dataset.cmd)));
document.querySelectorAll('[data-speed]').forEach((b) => b.addEventListener('click', () => setState({ speed: Number(b.dataset.speed) })));
document.querySelectorAll('[data-key]').forEach((el) => {
  el.addEventListener(el.type === 'range' ? 'input' : 'change', () => {
    const v = el.type === 'checkbox' ? el.checked : 'num' in el.dataset ? Number(el.value) : el.value;
    state[el.dataset.key] = v;
    setState({ [el.dataset.key]: v });
    renderLabels();
  });
});
$('speed').addEventListener('input', (e) => setState({ speed: Number(e.target.value) }));
$('bpm').addEventListener('change', (e) => {
  const base = status.baseTempo || 0;
  const bpm = Number(e.target.value);
  if (base && bpm > 0) setState({ speed: Math.round((bpm / base) * 1000) / 1000 });
});
const setLoop = () => setState({ loopFrom: Number($('loopFrom').value) || 0, loopTo: Number($('loopTo').value) || 0, loop: true });
$('loopFrom').addEventListener('change', setLoop);
$('loopTo').addEventListener('change', setLoop);
$('loopAll').addEventListener('click', () => setState({ loopFrom: 0, loopTo: 0 }));
$('track').addEventListener('change', (e) => setState({ track: Number(e.target.value) }));
$('rescan').addEventListener('click', () => { meta = {}; saveMeta(); loadLibrary(true); });
$('progress').addEventListener('click', (e) => {
  if (!status.duration) return;
  const r = e.currentTarget.getBoundingClientRect();
  command('seek', { step: ((e.clientX - r.left) / r.width) * status.duration });
});

// ------------------------------------------------------------ rendering state

function renderState() {
  for (const el of document.querySelectorAll('[data-key]')) {
    const v = state[el.dataset.key];
    if (v === undefined || document.activeElement === el) continue;
    if (el.type === 'checkbox') el.checked = !!v; else el.value = v;
  }
  const speed = Number(state.speed) || 1;
  if (document.activeElement !== $('speed')) $('speed').value = speed;
  if (document.activeElement !== $('loopFrom')) $('loopFrom').value = state.loopFrom || '';
  if (document.activeElement !== $('loopTo')) $('loopTo').value = state.loopTo || '';
  document.querySelectorAll('[data-speed]').forEach((b) => b.classList.toggle('active', Math.abs(Number(b.dataset.speed) - speed) < 0.001));
  renderLabels();
  renderTempo();
  markCurrent();
}

function renderLabels() {
  $('speedLabel').textContent = (Number(state.speed) || 1).toFixed(2) + '×';
  $('zoomLabel').textContent = (Number(state.zoom) || 1).toFixed(2) + '×';
  $('offsetLabel').textContent = `${Number(state.avOffset) || 0} ms`;
}

function renderTempo() {
  const base = status.baseTempo || 0;
  const speed = Number(state.speed) || 1;
  if (document.activeElement !== $('bpm')) $('bpm').value = base ? Math.round(base * speed) : '';
  const chips = $('rangeChips');
  chips.textContent = '';
  const range = status.fileId === state.fileId ? status.range : null;
  if (range && base) {
    const mid = Math.round((range[0] + range[1]) / 2);
    for (const [label, bpm] of [['min', range[0]], ['mid', mid], ['max', range[1]]]) {
      const b = document.createElement('button');
      b.textContent = `${label} ${bpm}`;
      b.title = `${bpm} BPM (range ${range[0]}–${range[1]})`;
      if (Math.round(base * speed) === bpm) b.classList.add('active');
      b.addEventListener('click', () => setState({ speed: Math.round((bpm / base) * 1000) / 1000 }));
      chips.append(b);
    }
  }
}

function renderStatus() {
  const conn = $('conn');
  if (!status.fileId && !status.duration && status.playing === undefined) { conn.textContent = 'add the overlay to a scene'; conn.className = 'pill warn'; }
  else { conn.textContent = status.playing ? 'playing' : 'ready'; conn.className = 'pill ok'; }
  const f = status.fileId ? describeFile(status.fileId) : null;
  $('nowTitle').textContent = f ? fileTitle(f) : '—';
  $('fmt').textContent = status.format ? status.format.toUpperCase() : '';
  const t = (s) => { s = Math.max(0, Math.floor(s || 0)); return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0'); };
  $('nowInfo').textContent = status.error ? '⚠ ' + status.error
    : status.bars ? `bar ${status.bar || 1}/${status.bars} · ${t(status.time)} / ${t(status.duration)}` : '';
  $('progressBar').style.width = status.duration ? ((100 * status.time) / status.duration).toFixed(1) + '%' : '0';
  $('playBtn').textContent = status.playing ? '❚❚' : '▶';
  const sel = $('track');
  const names = status.tracks || [];
  if (sel.dataset.names !== names.join('|')) {
    sel.dataset.names = names.join('|');
    sel.textContent = '';
    names.forEach((n, i) => sel.append(new Option(`${i + 1}. ${n}`, i)));
  }
  sel.value = state.track || 0;
  sel.disabled = names.length < 2;
  renderTempo();
}

// ------------------------------------------------------------ library and search

async function loadLibrary(rescan) {
  $('count').textContent = rescan ? 'Scanning…' : 'Loading library…';
  try {
    const d = await fetch('/api/library' + (rescan ? '?rescan=1' : '')).then((r) => r.json());
    files = d.files.map((f) => describeFile(f.id));
    $('libPath').textContent = d.root || 'No tab folder set: pick one in Tools > Scripts > tab-player.lua';
    filter();
    indexInBackground();
  } catch {
    $('count').textContent = 'Not connected. Is tab-player.lua loaded in Tools > Scripts?';
  }
}

function matches(f, tokens) {
  const m = meta[f.id] || {};
  const hay = [f.name, f.group, m.t, m.d].join(' ').toLowerCase();
  return tokens.every((t) => {
    let x;
    if ((x = /^(\d{1,3})$/.exec(t))) return f.day === Number(x[1]);
    if ((x = /^(\d{1,3})-(\d{1,3})$/.exec(t))) return f.day != null && f.day >= Number(x[1]) && f.day <= Number(x[2]);
    return hay.includes(t);
  });
}

function filter() {
  const q = $('search').value.toLowerCase().replace(/\bday\b/g, ' ').trim();
  const tokens = q.split(/\s+/).filter(Boolean);
  shown = tokens.length ? files.filter((f) => matches(f, tokens)) : files;
  cursor = Math.max(0, shown.findIndex((f) => f.id === state.fileId));
  $('count').textContent = files.length
    ? `${shown.length} of ${files.length} tabs · Enter loads · Ctrl+Enter plays`
    : 'No tabs found. Pick your tab folder in Tools > Scripts > tab-player.lua';
  renderList();
}

function renderList() {
  const list = $('list');
  const frag = document.createDocumentFragment();
  shown.forEach((f, i) => {
    const li = document.createElement('li');
    li.dataset.id = f.id;
    li.setAttribute('role', 'option');
    const m = meta[f.id];
    if (m?.d) li.title = m.d;
    const day = document.createElement('span');
    day.className = 'day';
    day.textContent = f.day != null ? String(f.day).padStart(3, '0') : '·';
    li.append(day);
    if (f.alt) { const a = document.createElement('span'); a.className = 'alt'; a.textContent = f.alt; li.append(a); }
    const topic = document.createElement('span');
    topic.className = 'topic';
    topic.textContent = f.day != null ? f.topic : f.name;
    li.append(topic);
    if (m?.r) { const t = document.createElement('span'); t.className = 'tempo'; t.textContent = `${m.r[0]}–${m.r[1]}`; li.append(t); }
    li.addEventListener('click', () => { cursor = i; pick(f, false); });
    li.addEventListener('dblclick', () => pick(f, true));
    frag.append(li);
  });
  list.replaceChildren(frag);
  markCurrent();
}

function markCurrent() {
  const items = $('list').children;
  for (let i = 0; i < items.length; i++) {
    items[i].classList.toggle('current', items[i].dataset.id === state.fileId);
    items[i].classList.toggle('cursor', i === cursor);
  }
}

function moveCursor(d) {
  if (!shown.length) return;
  cursor = Math.max(0, Math.min(shown.length - 1, cursor + d));
  markCurrent();
  $('list').children[cursor]?.scrollIntoView({ block: 'nearest' });
}

function pick(f, play) {
  setState({ fileId: f.id, loopFrom: 0, loopTo: 0, track: 0 });
  if (play) setTimeout(() => command('restart'), 150);
}

const keyNav = (e) => {
  if (e.key === 'ArrowDown') { e.preventDefault(); moveCursor(1); }
  else if (e.key === 'ArrowUp') { e.preventDefault(); moveCursor(-1); }
  else if (e.key === 'Enter' && shown[cursor]) { e.preventDefault(); pick(shown[cursor], e.ctrlKey || e.shiftKey); }
};
$('search').addEventListener('input', filter);
$('search').addEventListener('keydown', (e) => { if (e.key === 'Escape') { $('search').value = ''; filter(); } else keyNav(e); });
$('list').addEventListener('keydown', keyNav);

document.addEventListener('keydown', (e) => {
  const el = document.activeElement;
  if (/^(INPUT|SELECT|TEXTAREA)$/.test(el?.tagName) && !['checkbox', 'range'].includes(el.type)) return;
  const map = { ' ': 'playPause', '[': 'prevDay', ']': 'nextDay', '+': 'speedUp', '=': 'speedUp', '-': 'speedDown', l: 'toggleLoop', m: 'toggleMetronome', c: 'toggleCountIn' };
  if (map[e.key]) { e.preventDefault(); command(map[e.key]); }
  else if (e.key === '/') { e.preventDefault(); $('search').focus(); $('search').select(); }
});

// Background index of tempo ranges and descriptions for search. Light: one file at a
// time, only while idle, cached in localStorage.
function loadMeta() { try { return JSON.parse(localStorage.getItem('tab-player-meta-v1') || '{}'); } catch { return {}; } }
let saveTimer = 0;
function saveMeta() { clearTimeout(saveTimer); saveTimer = setTimeout(() => { try { localStorage.setItem('tab-player-meta-v1', JSON.stringify(meta)); } catch { /* full or blocked */ } }, 500); }
let indexing = false;
async function indexInBackground() {
  if (indexing) return;
  indexing = true;
  const todo = files.filter((f) => !meta[f.id]);
  let changed = 0;
  for (const f of todo) {
    await new Promise((r) => (window.requestIdleCallback ? requestIdleCallback(r, { timeout: 500 }) : setTimeout(r, 30)));
    try {
      const bytes = new Uint8Array(await (await fetch('/api/file?id=' + encodeURIComponent(f.id))).arrayBuffer());
      const s = await loadScore(bytes);
      const n = parseNotes(s.notices);
      meta[f.id] = { r: scoreRange(s), d: n.description || '', t: [s.title, s.subTitle, s.artist].filter(Boolean).join(' ') };
    } catch {
      meta[f.id] = { r: null, d: '', t: '' };
    }
    if (++changed % 40 === 0) { saveMeta(); renderList(); }
  }
  if (changed) { saveMeta(); renderList(); }
  indexing = false;
}

// ------------------------------------------------------------ events

connect('dock', {
  state: (s) => {
    const prev = state.fileId;
    state = { ...DEFAULTS, ...s };
    renderState();
    if (prev !== state.fileId) {
      const i = shown.findIndex((f) => f.id === state.fileId);
      if (i >= 0) { cursor = i; markCurrent(); $('list').children[i]?.scrollIntoView({ block: 'nearest' }); }
    }
  },
  status: (s) => { status = s; renderStatus(); },
  library: () => loadLibrary(false),
  error: () => { $('conn').textContent = 'OBS script not running'; $('conn').className = 'pill warn'; },
});
loadLibrary(false);
