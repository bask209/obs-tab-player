// SPDX-License-Identifier: Apache-2.0
// Tab view: hosts the rendered SVG, moves the cursor and scrolls with transforms only,
// so the browser never re-lays out or repaints the notation while playing.

import { renderTrack } from '../core/render.js';

const LAYOUT_KEYS = ['layout', 'zoom', 'rhythm', 'labels', 'barNumbers', 'track'];

export class TabView {
  constructor(root) {
    this.root = root;
    root.classList.add('tabview');
    root.innerHTML = '<div class="tab-viewport"><div class="tab-stage"><div class="tab-svg"></div><div class="tab-cursor-bar"></div><div class="tab-cursor-beat"></div></div></div>';
    this.viewport = root.querySelector('.tab-viewport');
    this.stage = root.querySelector('.tab-stage');
    this.svgHost = root.querySelector('.tab-svg');
    this.barEl = root.querySelector('.tab-cursor-bar');
    this.beatEl = root.querySelector('.tab-cursor-beat');
    this.score = null;
    this.timeline = null;
    this.geo = null;
    this.opts = {};
    this.layoutKey = '';
    this.width = 0;
    this.last = { beat: -1, sys: -1, scrollX: -1, scrollY: -1 };
    this.noteEls = new Map();
    this.lit = null;
    let pending = 0;
    this.resizeObserver = new ResizeObserver(() => {
      cancelAnimationFrame(pending);
      pending = requestAnimationFrame(() => {
        const w = this.viewport.clientWidth;
        if (Math.abs(w - this.width) > 2 && this.opts.layout !== 'horizontal') this.render();
        else this.place(this.lastTime ?? 0, true);
      });
    });
    this.resizeObserver.observe(this.viewport);
  }

  setScore(score, timeline) {
    this.score = score;
    this.timeline = timeline;
    this.render();
  }

  setOptions(o) {
    this.opts = o;
    const cls = this.root.classList;
    for (const c of [...cls]) if (/^(theme|font|cursor)-/.test(c)) cls.remove(c);
    cls.add('theme-' + (o.theme || 'light'), 'font-' + (o.font || 'sans'), 'cursor-' + (o.cursor || 'both'));
    cls.toggle('strings-color', !!o.colorStrings);
    cls.toggle('bold', !!o.bold);
    cls.toggle('no-highlight', !o.highlightNotes);
    const key = LAYOUT_KEYS.map((k) => o[k]).join('|');
    if (key !== this.layoutKey) {
      this.layoutKey = key;
      if (this.score) this.render();
    }
  }

  render() {
    if (!this.score) { this.svgHost.innerHTML = ''; return; }
    const o = this.opts;
    this.width = this.viewport.clientWidth;
    const r = renderTrack(this.score, o.track || 0, {
      width: Math.max(320, this.width),
      mode: o.layout === 'horizontal' ? 'horizontal' : 'page',
      scale: Number(o.zoom) || 1,
      rhythm: o.rhythm !== false,
      labels: o.labels || 'tab',
      barNumbers: o.barNumbers !== false,
    });
    this.svgHost.innerHTML = r.svg;
    this.geo = r;
    this.noteEls.clear();
    for (const el of this.svgHost.querySelectorAll('[data-k]')) this.noteEls.set(Number(el.dataset.k), el);
    this.lit = null;
    this.last = { beat: -1, sys: -1, scrollX: -1, scrollY: -1 };
    this.place(this.lastTime ?? 0, true);
  }

  // Move the cursor to a song time (seconds at speed 1).
  place(time, force = false) {
    this.lastTime = time;
    const tl = this.timeline;
    const geo = this.geo;
    if (!tl || !geo || !tl.beats.length) return;
    const beats = tl.beats;
    // binary search for the beat that contains `time`
    let lo = 0;
    let hi = beats.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (beats[mid].t <= time + 1e-6) lo = mid; else hi = mid - 1;
    }
    const b = beats[lo];
    const bar = geo.bars[b.bar];
    if (!bar) return;
    const sys = geo.systems[bar.sys];
    const key = b.bar * 4096 + b.beat;
    const x0 = geo.beats.get(key) ?? bar.x0;
    // glide toward the next beat if it is on the same row
    const nb = beats[lo + 1];
    let x = x0;
    if (nb && b.dur > 0) {
      const nbar = geo.bars[nb.bar];
      let x1 = nbar && nbar.sys === bar.sys ? geo.beats.get(nb.bar * 4096 + nb.beat) : null;
      if (x1 == null || x1 < x0) x1 = bar.x1;
      const f = Math.min(1, Math.max(0, (time - b.t) / b.dur));
      x = x0 + (x1 - x0) * f;
    }
    const top = sys.top - 10 * (this.opts.zoom || 1);
    const h = sys.bottom - sys.top + 20 * (this.opts.zoom || 1);
    this.beatEl.style.transform = `translate3d(${x - 1.5}px, ${top}px, 0) scaleY(${h / 100})`;
    this.cursorX = x - this.last.scrollX;
    if (lo !== this.last.beat || force) {
      this.barEl.style.transform = `translate3d(${bar.x0}px, ${top}px, 0) scale(${(bar.x1 - bar.x0) / 100}, ${h / 100})`;
      if (this.opts.highlightNotes !== false) {
        if (this.lit) this.lit.classList.remove('on');
        this.lit = this.noteEls.get(key) || null;
        if (this.lit) this.lit.classList.add('on');
      }
      this.last.beat = lo;
    }
    this.scrollTo(x, sys, force);
  }

  scrollTo(x, sys, force) {
    const vw = this.viewport.clientWidth;
    const vh = this.viewport.clientHeight;
    let sx = 0;
    let sy = 0;
    if (this.opts.layout === 'horizontal') {
      sx = Math.max(0, Math.min(x - vw * 0.3, this.geo.width - vw));
      sy = this.geo.height > vh ? Math.max(0, Math.min(sys.top - vh / 3, this.geo.height - vh)) : 0;
    } else {
      // keep the current row near the top, showing as much of what follows as fits
      sy = Math.max(0, Math.min(sys.y - 6, this.geo.height - vh));
      if (this.geo.height <= vh) sy = 0;
    }
    if (force || Math.abs(sx - this.last.scrollX) > 0.5 || sy !== this.last.scrollY) {
      const smooth = this.opts.layout !== 'horizontal' && !force && sy !== this.last.scrollY;
      this.stage.classList.toggle('glide', smooth);
      this.stage.style.transform = `translate3d(${-Math.round(sx)}px, ${-Math.round(sy)}px, 0)`;
      this.last.scrollX = sx;
      this.last.scrollY = sy;
    }
  }
}
