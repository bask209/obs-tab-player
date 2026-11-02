// SPDX-License-Identifier: Apache-2.0
// The tab on the website: the plugin's own layout (from the WebAssembly core) drawn as SVG.
// The cursor moves and the view scrolls with transforms only, so the browser never re-lays
// out or repaints the notation while playing. Colours come from CSS classes (tab.css).
import { layoutSvg } from './app/core.mjs';

export class TabView {
  constructor(root, core) {
    this.root = root;
    this.core = core;
    root.classList.add('tabview');
    root.innerHTML = '<div class="tab-viewport"><div class="tab-stage"><div class="tab-svg"></div><div class="tab-cursor-bar"></div><div class="tab-cursor-beat"></div></div></div>';
    this.viewport = root.querySelector('.tab-viewport');
    this.stage = root.querySelector('.tab-stage');
    this.svgHost = root.querySelector('.tab-svg');
    this.barEl = root.querySelector('.tab-cursor-bar');
    this.beatEl = root.querySelector('.tab-cursor-beat');
    this.timeline = null;
    this.geo = null;
    this.opts = { zoom: 1, layout: 'horizontal' };
    this.width = 0;
    this.last = { beat: -1, sys: null, scrollX: -1, scrollY: -1, target: 0 };
    this.noteEls = new Map();
    this.lit = null;
    let pending = 0;
    new ResizeObserver(() => {
      cancelAnimationFrame(pending);
      pending = requestAnimationFrame(() => {
        const w = this.viewport.clientWidth;
        if (Math.abs(w - this.width) > 2 && this.opts.layout !== 'horizontal') this.render();
        else this.place(this.lastTime ?? 0, true);
      });
    }).observe(this.viewport);
  }

  setOptions(o) { this.opts = { ...this.opts, ...o }; }

  // The core has a tab open: draw it and follow this timeline.
  show(timeline) {
    this.timeline = timeline;
    this.render();
  }

  render() {
    if (!this.timeline) { this.svgHost.innerHTML = ''; return; }
    const o = this.opts;
    this.width = this.viewport.clientWidth;
    const scale = Number(o.zoom) || 1;
    const lay = this.core.layout({ width: Math.max(320, this.width), horizontal: o.layout === 'horizontal', scale });
    this.svgHost.innerHTML = layoutSvg(lay, scale);
    this.geo = lay;
    this.noteEls.clear();
    for (const el of this.svgHost.querySelectorAll('[data-k]')) this.noteEls.set(Number(el.dataset.k), el);
    this.lit = null;
    this.last = { beat: -1, sys: null, scrollX: -1, scrollY: -1, target: 0 };
    this.place(this.lastTime ?? 0, true);
  }

  // Beat and geometry at a song time (seconds at speed 1).
  locate(time) {
    const tl = this.timeline;
    const geo = this.geo;
    if (!tl || !geo || !tl.beats.length) return null;
    const beats = tl.beats;
    let lo = 0;
    let hi = beats.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (beats[mid].t <= time + 1e-6) lo = mid; else hi = mid - 1;
    }
    const b = beats[lo];
    const bar = geo.bars[b.bar];
    if (!bar) return null;
    const sys = geo.systems[bar.sys];
    const key = b.bar * 4096 + b.beat;
    const x0 = geo.beats.get(key) ?? bar.x0;
    const nb = beats[lo + 1];
    const nbar = nb && geo.bars[nb.bar];
    let x1 = nbar && nbar.sys === bar.sys ? geo.beats.get(nb.bar * 4096 + nb.beat) : null;
    if (x1 == null || x1 < x0) x1 = bar.x1;
    const f = b.dur > 0 ? Math.min(1, Math.max(0, (time - b.t) / b.dur)) : 0;
    const zoom = this.opts.zoom || 1;
    return { lo, bar, sys, key, x0, x1, x: x0 + (x1 - x0) * f, end: b.t + b.dur, top: sys.top - 10 * zoom, h: sys.bottom - sys.top + 20 * zoom };
  }

  // Static placement (paused, seeking, resizing): no animation.
  place(time, force = false) {
    this.lastTime = time;
    const L = this.locate(time);
    if (!L) return;
    this.setCursor(L, L.x, 0, true);
    this.setBeat(L, force);
    this.setScroll(L, L.x, 0, force);
  }

  // While playing: the cursor glides to the next beat with a linear CSS transition, so no
  // script runs between beats. Returns the real time in seconds until the next beat.
  follow(time, speed) {
    this.lastTime = time;
    const L = this.locate(time);
    if (!L) return 0.25;
    const remaining = Math.max(0.005, (L.end - time) / speed);
    const continuous = L.lo === this.last.beat + 1 && L.sys === this.last.sys && Math.abs(L.x0 - this.last.target) < 1;
    if (!continuous && L.lo !== this.last.beat) this.setCursor(L, L.x, 0, true);
    this.setBeat(L, false);
    this.setCursor(L, L.x1, remaining, false);
    this.setScroll(L, L.x1, remaining, false);
    this.last.target = L.x1;
    return remaining;
  }

  setCursor(L, x, seconds, jump) {
    const el = this.beatEl;
    el.style.transition = seconds > 0 ? `transform ${seconds.toFixed(3)}s linear` : 'none';
    el.style.transform = `translate3d(${(x - 1.5).toFixed(1)}px, ${L.top.toFixed(1)}px, 0) scaleY(${(L.h / 100).toFixed(4)})`;
    if (jump) { void el.offsetWidth; this.last.target = x; } // commit the jump before a following transition starts
  }

  // Bar highlight and lit notes change only when the beat changes.
  setBeat(L, force) {
    if (L.lo === this.last.beat && !force) return;
    const { bar } = L;
    this.barEl.style.transform = `translate3d(${bar.x0}px, ${L.top.toFixed(1)}px, 0) scale(${((bar.x1 - bar.x0) / 100).toFixed(4)}, ${(L.h / 100).toFixed(4)})`;
    const el = this.noteEls.get(L.key) || null;
    if (el !== this.lit) {
      if (this.lit) this.lit.classList.remove('on');
      if (el) el.classList.add('on');
      this.lit = el;
    }
    this.last.beat = L.lo;
    this.last.sys = L.sys;
  }

  setScroll(L, x, seconds, force) {
    const vw = this.viewport.clientWidth;
    const vh = this.viewport.clientHeight;
    let sx = 0;
    let sy;
    let transition = 'none';
    if (this.opts.layout === 'horizontal') {
      sx = Math.max(0, Math.min(x - vw * 0.3, this.geo.width - vw));
      sy = this.geo.height > vh ? Math.max(0, Math.min(L.sys.top - vh / 3, this.geo.height - vh)) : 0;
      if (seconds > 0) transition = `transform ${seconds.toFixed(3)}s linear`;
    } else {
      // keep the current row near the top, showing as much of what follows as fits
      sy = this.geo.height <= vh ? 0 : Math.max(0, Math.min(L.sys.y - 6, this.geo.height - vh));
      if (sy === this.last.scrollY && !force) return;
      if (!force) transition = 'transform 0.35s ease';
    }
    if (!force && Math.abs(sx - this.last.scrollX) < 0.5 && sy === this.last.scrollY) return;
    this.stage.style.transition = transition;
    this.stage.style.transform = `translate3d(${-sx.toFixed(1)}px, ${-Math.round(sy)}px, 0)`;
    this.last.scrollX = sx;
    this.last.scrollY = sy;
  }
}
