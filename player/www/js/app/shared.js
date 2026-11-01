// SPDX-License-Identifier: Apache-2.0
// Shared by the overlay and the dock: defaults, server connection, library helpers.

export const DEFAULTS = {
  fileId: null,
  track: 0,
  speed: 1,
  loop: true,
  loopFrom: 0,
  loopTo: 0,
  countIn: true,
  metronome: false,
  volume: 0.9,
  clickVolume: 0.8,
  tone: 'clean', // clean | warm | crunch
  theme: 'light', // light | dark | glass | clear | neon | paper
  font: 'sans', // sans | rounded | mono | serif
  colorStrings: false,
  bold: false,
  cursor: 'both', // both | line | bar | none
  highlightNotes: true,
  layout: 'page', // page | horizontal
  zoom: 1.2,
  rhythm: true,
  labels: 'tab', // tab | names | none
  barNumbers: true,
  showNotes: true,
  showAllNotes: false,
  panel: 'top', // top | bottom | hidden
  avOffset: 0, // ms, positive = cursor later
};

export function connect(role, handlers) {
  let es = null;
  let retry = 500;
  const open = () => {
    es = new EventSource('/events?role=' + role);
    for (const [event, fn] of Object.entries(handlers)) {
      if (event === 'open' || event === 'error') continue;
      es.addEventListener(event, (e) => fn(JSON.parse(e.data || '{}')));
    }
    es.onopen = () => { retry = 500; handlers.open?.(); };
    es.onerror = () => {
      handlers.error?.();
      es.close();
      setTimeout(open, retry);
      retry = Math.min(retry * 2, 5000);
    };
  };
  open();
}

export function command(type, extra = {}) {
  return fetch('/api/command', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ type, ...extra }),
  }).catch(() => {});
}

export const setState = (values) => command('set', { values });

// "Day 004 - A - Arpeggios.gp5" -> { day: 4, alt: 'A', topic: 'Arpeggios' }
export function describeFile(id) {
  const base = id.split('/').pop().replace(/\.[^.]+$/, '');
  const group = id.includes('/') ? id.slice(0, id.lastIndexOf('/')) : '';
  const m = /^Day\s*(\d{1,3})(?:\s*-\s*([A-Z]))?\s*-\s*(.+)$/i.exec(base);
  if (m) return { id, name: base, day: Number(m[1]), alt: (m[2] || '').toUpperCase(), topic: m[3].trim(), group };
  return { id, name: base, day: null, alt: '', topic: base, group };
}

export function fileTitle(f) {
  if (!f) return '';
  if (f.day != null) return `Day ${String(f.day).padStart(3, '0')} · ${f.topic}${f.alt ? ' · ' + f.alt : ''}`;
  return f.name;
}

// Pulls the useful labelled lines and a tempo range out of free-text score notes.
export function parseNotes(text) {
  const lines = String(text || '').split(/\r?\n/).map((l) => l.trim()).filter(Boolean);
  const out = { description: '', tip: '', range: null, extra: [], lines };
  for (const l of lines) {
    let m;
    if ((m = /^(?:Music\s+)?Description:\s*(.+)$/i.exec(l))) out.description = m[1];
    else if ((m = /^Tip:\s*(.+)$/i.exec(l))) out.tip = m[1];
    else if (/^(Version|Alternative|Primary)\b/i.test(l)) out.extra.push(l);
    if (!out.range && (m = /(\d{2,3})\s*[-–]\s*(\d{2,3})\s*BPM/i.exec(l))) out.range = [Number(m[1]), Number(m[2])];
  }
  return out;
}

export function scoreRange(score) {
  const n = parseNotes(score.notices);
  if (n.range) return n.range;
  const m = /(\d{2,3})\s*[-–]\s*(\d{2,3})/.exec(score.tempoLabel || '');
  return m ? [Number(m[1]), Number(m[2])] : null;
}
