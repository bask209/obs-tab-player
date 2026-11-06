// SPDX-License-Identifier: Apache-2.0
// The engine (plugin/src/core) compiled to WebAssembly, for the website and the tests.
// tools/build-web.mjs puts this file next to tabcore.mjs and tabcore.wasm.
import createModule from './tabcore.mjs';

export const Role = {
  Staff: 0, BarLine: 1, BarThick: 2, RepeatDot: 3, Note: 4, NoteSoft: 5, Clef: 6, StringName: 7, TimeSig: 8, BarNumber: 9, Marker: 10,
  Chord: 11, BeatText: 12, SmallText: 13, Technique: 14, TechniqueFill: 15, Dashed: 16, Stem: 17, Beam: 18, Rest: 19, Arrow: 20,
};
export const PrimType = { Line: 0, Rect: 1, Text: 2, Path: 3, Circle: 4 };

// ---- SVG of a layout (colours come from CSS classes; see docs/tab.css)

const CLASS = ['st', 'bl', 'bt', 'rd', 'n', 'n soft', 'clef', 'sname', 'ts', 'bn', 'mk', 'cn', 'btx', 'sm', 'tq', 'tqf', 'dash', 'stem', 'beam', 'rest', 'arrow'];
const ANCHOR = ['start', 'middle', 'end'];
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
const r1 = (x) => Math.round(x * 10) / 10;

// Only what differs from the defaults in tab.css is written: text is var(--fs), centred; strokes are 1.
function primSvg(p, font, attr = '') {
  const byString = p.string >= 0 && (p.role === Role.Staff || p.role === Role.Note || p.role === Role.NoteSoft);
  const cls = CLASS[p.role] + (byString ? ` s${p.string}` : '');
  const sw = p.role === Role.Staff || p.role === Role.BarLine || p.stroke === 1 ? '' : ` stroke-width="${r1(p.stroke)}"`;
  switch (p.type) {
    case PrimType.Line:
      return `<line class="${cls}"${attr}${sw} x1="${r1(p.x0)}" y1="${r1(p.y0)}" x2="${r1(p.x1)}" y2="${r1(p.y1)}"/>`;
    case PrimType.Rect:
      return `<rect class="${cls}"${attr} x="${r1(p.x0)}" y="${r1(p.y0)}" width="${r1(p.x1 - p.x0)}" height="${r1(p.y1 - p.y0)}"/>`;
    case PrimType.Circle:
      return `<circle class="${cls}"${attr} cx="${r1(p.x0)}" cy="${r1(p.y0)}" r="${r1(p.x1)}"/>`;
    case PrimType.Path:
      return `<path class="${cls}${p.role === Role.Arrow || p.role === Role.TechniqueFill ? '' : ' line'}"${attr}${sw} d="${p.path}"/>`;
    default: {
      const style = (Math.abs(p.size - font) > 0.01 ? `font-size:${r1(p.size)}px;` : '') + (p.align !== 1 ? `text-anchor:${ANCHOR[p.align]};` : '')
        + (p.bold && p.role !== Role.Note ? 'font-weight:700;' : '') + (p.italic ? 'font-style:italic;' : '');
      return `<text class="${cls}${p.bold && p.role === Role.Note ? ' b' : ''}"${attr} x="${r1(p.x0)}" y="${r1(p.y0)}"${style ? ` style="${style}"` : ''}>${esc(p.text)}</text>`;
    }
  }
}

export function layoutSvg(lay, scale) {
  const font = 12.5 * scale;
  // the notes of one beat go in one group, which the cursor lights up
  const groups = new Map();
  for (const p of lay.prims) if (p.beatKey >= 0 && p.type === PrimType.Text) (groups.get(p.beatKey) || groups.set(p.beatKey, []).get(p.beatKey)).push(p);
  const parts = [];
  for (const p of lay.prims) {
    if (p.beatKey >= 0 && p.type === PrimType.Text) {
      const g = groups.get(p.beatKey);
      if (g[0] === p) parts.push(`<g data-k="${p.beatKey}">${g.map((x) => primSvg(x, font)).join('')}</g>`);
      continue;
    }
    parts.push(primSvg(p, font, p.beatKey >= 0 ? ` data-k="${p.beatKey}"` : ''));
  }
  return `<svg xmlns="http://www.w3.org/2000/svg" class="tab" width="${lay.width}" height="${lay.height}" viewBox="0 0 ${lay.width} ${lay.height}" style="--fs:${r1(font)}px">${parts.join('')}</svg>`;
}

export async function openCore() {
  return new Core(await createModule());
}

export class Core {
  constructor(m) {
    this.m = m;
  }

  #json(ptr) {
    return JSON.parse(this.m.UTF8ToString(ptr));
  }

  // Opens a tab file (bytes). Returns { track, tracks: [{ percussion }] }; throws on a bad file.
  load(bytes, sampleRate = 48000) {
    const data = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    const at = this.m._tc_input(data.length);
    this.m.HEAPU8.set(data, at);
    const r = this.#json(this.m._tc_load(sampleRate));
    if (r.error) throw new Error(r.error);
    return r;
  }

  selectTrack(track) { this.m._tc_select_track(track); }
  score() { return this.#json(this.m._tc_score()); }
  timeline() { return this.#json(this.m._tc_timeline()); }

  // { width, height, prims, bars, systems, beats: Map(beatKey -> x) }. Each primitive is
  // { type, role, x0, y0, x1, y1, size, align, bold, italic, string, beatKey, text, path, stroke }.
  layout({ width = 900, horizontal = false, scale = 1, rhythm = true, barNumbers = true, labels = 'tab' } = {}) {
    const l = this.#json(this.m._tc_layout(width, horizontal ? 1 : 0, scale, rhythm ? 1 : 0, barNumbers ? 1 : 0, labels === 'names' ? 1 : labels === 'none' ? 2 : 0));
    l.prims = l.prims.map(([type, role, x0, y0, x1, y1, size, align, bold, italic, string, beatKey, text, path, stroke]) => ({
      type, role, x0, y0, x1, y1, size, align, bold: !!bold, italic: !!italic, string, beatKey, text, path, stroke,
    }));
    l.beats = new Map(l.beats);
    return l;
  }

  // Playback with the plugin's synth. render(n) returns { part, clicks } as Float32Arrays.
  play(from, speed = 1, countIn = false, loop = false) { this.m._tc_play(from, speed, countIn ? 1 : 0, loop ? 1 : 0); }
  pause() { this.m._tc_pause(); }
  seek(t) { this.m._tc_seek(t); }
  setSpeed(s) { this.m._tc_speed(s); }
  setLoop(on, a = 0, b = 0) { this.m._tc_loop(on ? 1 : 0, a, b); }
  setMetronome(on) { this.m._tc_metronome(on ? 1 : 0); }
  // between loop repeats: 'none', 'rest' (one silent bar, counted) or 'count' (one bar of clicks)
  setLoopGap(mode) { this.m._tc_loop_gap(mode === 'count' ? 2 : mode === 'rest' ? 1 : 0); }
  get playing() { return this.m._tc_playing() === 1; }
  get rendered() { return this.m._tc_rendered(); } // samples rendered so far
  positionAt(sample) { return this.m._tc_position(sample); }
  countInAt(sample) { return this.m._tc_count_in(sample); }
  render(n) {
    this.m._tc_render(n);
    const part = new Float32Array(this.m.HEAPF32.buffer, this.m._tc_part(), n).slice();
    const clicks = new Float32Array(this.m.HEAPF32.buffer, this.m._tc_clicks(), n).slice();
    return { part, clicks };
  }
}
