// SPDX-License-Identifier: Apache-2.0
// Light plucked-string synthesizer (Karplus-Strong) on the Web Audio API.
// Each pitch is rendered once into a small mono buffer with the tone (body EQ, treble,
// overdrive) already baked in, then played by native AudioBufferSourceNodes. The live
// graph per note is just source -> gain, so the audio thread does almost no work, and
// the context is suspended whenever nothing plays.

const SAMPLE_RATE = 32000; // plenty for guitar, a third less memory than 48 kHz
const RING_SECONDS = 3.2;

export class Synth {
  constructor() {
    this.ctx = null;
    this.cache = new Map();
    this.voices = new Map(); // track*16+string -> { src, gain }
    this.live = new Set();
    this.tone = 'clean';
    this.volume = 0.9;
    this.clickVolume = 0.8;
  }

  ensure() {
    if (this.ctx) return this.ctx;
    const AC = window.AudioContext || window.webkitAudioContext;
    // "playback" asks the browser for larger audio buffers: fewer wake-ups, lower CPU.
    // 48 kHz matches OBS; devices running at 96/192 kHz would otherwise multiply the audio work.
    try { this.ctx = new AC({ latencyHint: 'playback', sampleRate: 48000 }); } catch { this.ctx = new AC({ latencyHint: 'playback' }); }
    const ctx = this.ctx;
    this.out = ctx.createGain();
    this.out.gain.value = this.volume;
    this.out.connect(ctx.destination);
    this.clickBus = ctx.createGain();
    this.clickBus.gain.value = this.clickVolume;
    this.clickBus.connect(ctx.destination);
    this.clickHi = this.renderClick(1760, 0.035);
    this.clickLo = this.renderClick(1175, 0.03);
    return ctx;
  }

  async resume() {
    const ctx = this.ensure();
    clearTimeout(this.sleepTimer);
    if (ctx.state !== 'running') await ctx.resume().catch(() => {});
    return ctx.state === 'running';
  }

  // Suspend the audio engine shortly after the last sound, so a paused player costs nothing.
  sleepSoon(delay = 600) {
    clearTimeout(this.sleepTimer);
    if (!this.ctx) return;
    this.sleepTimer = setTimeout(() => {
      if (this.ctx.state === 'running' && !this.busy) this.ctx.suspend().catch(() => {});
    }, delay);
  }

  get now() { return this.ctx ? this.ctx.currentTime : 0; }

  setVolume(v) { this.volume = v; if (this.out) this.out.gain.setTargetAtTime(v, this.ctx.currentTime, 0.02); }
  setClickVolume(v) { this.clickVolume = v; if (this.clickBus) this.clickBus.gain.setTargetAtTime(v, this.ctx.currentTime, 0.02); }

  // Tone is baked into the string buffers, so changing it re-renders them.
  setTone(tone) {
    if (tone === this.tone) return;
    this.tone = tone;
    this.cache.clear();
    if (this.ctx && this.lastNotes) this.prepare(this.lastNotes);
  }

  renderClick(freq, len) {
    const n = Math.round(SAMPLE_RATE * len);
    const buf = this.ctx.createBuffer(1, n, SAMPLE_RATE);
    const d = buf.getChannelData(0);
    for (let i = 0; i < n; i++) {
      const t = i / SAMPLE_RATE;
      d[i] = Math.sin(2 * Math.PI * freq * t) * Math.exp(-t * 90) * 0.9;
    }
    return buf;
  }

  // Pre-render every pitch/variant a timeline needs, so playback never stalls.
  prepare(notes) {
    this.ensure();
    this.lastNotes = notes;
    const seen = new Set();
    for (const n of notes) {
      const key = n.midi * 4 + variantIndex(n.kind);
      if (!seen.has(key)) { seen.add(key); this.buffer(n.midi, n.kind); }
    }
  }

  buffer(midi, kind) {
    const key = midi * 4 + variantIndex(kind);
    let b = this.cache.get(key);
    if (!b) {
      b = renderString(this.ctx, midi, kind, this.tone);
      this.cache.set(key, b);
    }
    return b;
  }

  // Free buffers that the current file does not use (keeps memory low on long sessions).
  trim(notes) {
    const keep = new Set(notes.map((n) => n.midi * 4 + variantIndex(n.kind)));
    for (const k of this.cache.keys()) if (!keep.has(k)) this.cache.delete(k);
  }

  click(when, down) {
    const src = this.ctx.createBufferSource();
    src.buffer = down ? this.clickHi : this.clickLo;
    src.connect(this.clickBus);
    src.start(when);
    this.track(src, null, when);
  }

  // Schedule one note. `speed` scales its duration and pitch-bend timing.
  note(ev, when, speed) {
    const ctx = this.ctx;
    const dur = ev.dur / speed;
    const src = ctx.createBufferSource();
    src.buffer = this.buffer(ev.midi, ev.kind);
    const gain = ctx.createGain();
    let vel = ev.vel * (ev.legato ? 0.55 : 1);
    if (ev.kind === 'dead') vel *= 0.7;
    gain.gain.value = vel;
    src.connect(gain);
    gain.connect(this.out);

    // pitch movements via playbackRate (no extra processing nodes)
    const rate = src.playbackRate;
    if (ev.slideIn) {
      rate.setValueAtTime(semis(ev.slideIn), when);
      rate.linearRampToValueAtTime(1, when + Math.min(0.08, dur / 2));
    }
    if (ev.bend && ev.bend.length) {
      rate.setValueAtTime(semis(ev.bend[0].semis), when);
      for (const p of ev.bend) rate.linearRampToValueAtTime(semis(p.semis), when + p.t / speed); // p.t: seconds at speed 1
    }
    if (ev.slideNext && ev.slideTarget) {
      const tail = Math.min(0.12, dur * 0.4);
      rate.setValueAtTime(1, when + dur - tail);
      rate.linearRampToValueAtTime(semis(ev.slideTarget - ev.midi), when + dur);
    } else if (ev.slideTarget) {
      rate.setValueAtTime(1, when);
      rate.linearRampToValueAtTime(semis(ev.slideTarget - ev.midi), when + dur);
    }
    if (ev.slideOut) {
      const tail = Math.min(0.15, dur * 0.5);
      rate.setValueAtTime(1, when + dur - tail);
      rate.linearRampToValueAtTime(semis(ev.slideOut), when + dur);
    }
    let lfo = null;
    if (ev.vibrato && dur > 0.15) {
      lfo = ctx.createOscillator();
      lfo.frequency.value = 5.5;
      const depth = ctx.createGain();
      depth.gain.value = 0.012;
      lfo.connect(depth);
      depth.connect(rate);
      lfo.start(when + 0.08);
      lfo.stop(when + dur + 0.05);
    }

    // one sound per string: fade the previous note on this string
    const key = ev.track * 16 + ev.string;
    const prev = this.voices.get(key);
    if (prev && prev.end > when) {
      prev.gain.gain.setTargetAtTime(0, when, 0.008);
      try { prev.src.stop(when + 0.05); } catch { /* already stopped */ }
    }
    const release = ev.kind === 'dead' ? 0.01 : 0.03;
    const end = when + dur;
    gain.gain.setValueAtTime(vel, Math.max(when, end - 0.001));
    gain.gain.setTargetAtTime(0, end, release);
    src.start(when);
    src.stop(Math.min(end + release * 6, when + src.buffer.duration * 2));
    const voice = { src, gain, end, start: when };
    this.voices.set(key, voice);
    this.track(src, gain, when);
  }

  track(src, gain, when) {
    const item = { src, gain, when };
    this.live.add(item);
    src.onended = () => {
      this.live.delete(item);
      src.disconnect();
      if (gain) gain.disconnect();
    };
  }

  // Stop everything that has not started yet (after a seek, speed change or pause).
  cancelFuture(from) {
    for (const item of this.live) {
      if (item.when >= from - 0.001) { try { item.src.stop(); } catch { /* not started */ } }
    }
  }

  // Quickly silence all notes.
  silence() {
    if (!this.ctx) return;
    const t = this.ctx.currentTime;
    for (const item of this.live) {
      if (item.gain) item.gain.gain.setTargetAtTime(0, t, 0.01);
      try { item.src.stop(t + 0.06); } catch { /* ignore */ }
    }
    this.voices.clear();
  }
}

const semis = (s) => Math.pow(2, s / 12);
const variantIndex = (kind) => (kind === 'muted' ? 1 : kind === 'dead' ? 2 : kind === 'harmonic' ? 3 : 0);

// Karplus-Strong with fractional-delay tuning, pick-position comb and per-pitch decay.
function renderString(ctx, midi, kind, tone = 'clean') {
  const sr = SAMPLE_RATE;
  const f = 440 * Math.pow(2, (midi - 69) / 12);
  const dead = kind === 'dead';
  const muted = kind === 'muted';
  const harmonic = kind === 'harmonic';
  const seconds = dead ? 0.09 : muted ? 0.45 : RING_SECONDS;
  const len = Math.round(sr * seconds);
  const buf = ctx.createBuffer(1, len, sr);
  const out = buf.getChannelData(0);

  // Loop delay = N (delay line) - 0.5 (two-point average of the two oldest samples) + allpass.
  // Keep the allpass delay in [0.15, 1.15) where it is well behaved.
  const period = sr / f;
  const N = Math.max(2, Math.floor(period + 0.5 - 0.15));
  const frac = period + 0.5 - N;
  const C = (1 - frac) / (1 + frac);

  // T60 shortens with pitch; palm mutes decay fast and lose highs.
  const t60 = muted ? 0.25 : dead ? 0.04 : Math.max(0.9, 4.2 - (midi - 40) * 0.07);
  const loss = Math.pow(10, -3 / (f * t60));
  const bright = muted ? 0.25 : harmonic ? 0.2 : 0.5; // averaging weight: lower = brighter

  // excitation: noise burst shaped by pick position (comb), slightly low-passed
  const line = new Float32Array(N + 2);
  let seed = midi * 9301 + 49297;
  const rnd = () => { seed = (seed * 16807) % 2147483647; return seed / 1073741823.5 - 1; };
  let lp = 0;
  for (let i = 0; i < N + 2; i++) {
    const x = harmonic ? Math.sin((2 * Math.PI * i * 2) / N) : rnd();
    lp += (x - lp) * (muted ? 0.35 : 0.7);
    line[i] = lp;
  }
  const pick = Math.max(1, Math.round(N * 0.13));
  for (let i = N + 1; i >= pick; i--) line[i] -= line[i - pick] * 0.9;
  let mean = 0;
  for (let i = 0; i < N + 2; i++) mean += line[i];
  mean /= N + 2;
  let peak = 0;
  for (let i = 0; i < N + 2; i++) { line[i] -= mean; peak = Math.max(peak, Math.abs(line[i])); }
  const norm = 0.5 / (peak || 1);

  // feedback loop
  let idx = 0;
  let apX = 0;
  let apY = 0;
  for (let i = 0; i < len; i++) {
    const cur = line[idx];
    const next = line[(idx + 1) % N];
    const avg = (1 - bright) * cur + bright * next;
    const y = C * avg + apX - C * apY; // allpass for fine tuning
    apX = avg;
    apY = y;
    const v = y * loss;
    line[idx] = v;
    idx = (idx + 1) % N;
    out[i] = cur * norm;
  }
  // tone, baked in: body resonance, optional overdrive, treble roll-off
  biquad(out, peaking(220, 0.9, 3, sr));
  if (tone === 'crunch') for (let i = 0; i < len; i++) out[i] = Math.tanh(out[i] * 4) * 0.7;
  biquad(out, lowpass(tone === 'warm' ? 3200 : tone === 'crunch' ? 4200 : 7500, 0.707, sr));
  // gentle fade at the very end to avoid a click
  const fade = Math.min(len, Math.round(sr * 0.05));
  for (let i = 0; i < fade; i++) out[len - 1 - i] *= i / fade;
  return buf;
}

// RBJ cookbook biquads, applied once while rendering a buffer.
function lowpass(f, q, sr) {
  const w = (2 * Math.PI * f) / sr; const c = Math.cos(w); const al = Math.sin(w) / (2 * q);
  return norm([(1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al]);
}
function peaking(f, q, db, sr) {
  const A = 10 ** (db / 40); const w = (2 * Math.PI * f) / sr; const c = Math.cos(w); const al = Math.sin(w) / (2 * q);
  return norm([1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A]);
}
const norm = ([b0, b1, b2, a0, a1, a2]) => [b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0];
function biquad(d, [b0, b1, b2, a1, a2]) {
  let x1 = 0; let x2 = 0; let y1 = 0; let y2 = 0;
  for (let i = 0; i < d.length; i++) {
    const x = d[i];
    const y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x; y2 = y1; y1 = y;
    d[i] = y;
  }
}
