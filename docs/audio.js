// SPDX-License-Identifier: Apache-2.0
// Plays the open tab with the plugin's own synth (in the WebAssembly core): blocks are rendered
// a little ahead and queued back to back in Web Audio, much as the plugin's audio thread hands
// 20 ms blocks to OBS. The part and the clicks have their own gain, like the two OBS faders.
const BLOCK = 2048;
const AHEAD = 0.2; // seconds rendered ahead of what you hear

export class Player {
  constructor(core) {
    this.core = core;
    this.ctx = new AudioContext({ latencyHint: 'interactive' });
    this.partGain = this.ctx.createGain();
    this.clickGain = this.ctx.createGain();
    this.partGain.connect(this.ctx.destination);
    this.clickGain.connect(this.ctx.destination);
    this.speed = 1;
    this.loop = true;
    this.countIn = true;
    this.base = 0; // audio-clock time of rendered sample 0
    this.queued = [];
    this.timer = 0;
    this.pausedAt = 0;
    this.playing = false;
    this.onstate = null;
  }

  get sampleRate() { return this.ctx.sampleRate; }
  set metronome(on) { this.core.setMetronome(on); }

  // The sample being heard now.
  heard() { return (this.ctx.currentTime - (this.ctx.outputLatency || 0) - this.base) * this.sampleRate; }
  position() { return this.playing ? this.core.positionAt(Math.max(0, this.heard())) : this.pausedAt; }
  countInBeat() { return this.playing ? this.core.countInAt(Math.max(0, this.heard())) : 0; }

  // A new tab is open in the core.
  reset() {
    this.stopQueued();
    this.playing = false;
    this.pausedAt = 0;
    this.onstate?.();
  }

  async play() {
    await this.ctx.resume();
    this.core.setLoop(this.loop);
    this.core.setSpeed(this.speed);
    this.core.play(this.pausedAt, this.speed, this.countIn, this.loop);
    this.base = this.ctx.currentTime + 0.05 - this.core.rendered / this.sampleRate;
    this.playing = true;
    this.pump();
    this.onstate?.();
  }

  pause() {
    if (!this.playing) return;
    this.pausedAt = this.position();
    this.playing = false;
    this.core.pause();
    this.core.seek(this.pausedAt);
    this.stopQueued();
    this.onstate?.();
  }

  setSpeed(s) {
    this.speed = s;
    this.core.setSpeed(s);
  }

  stopQueued() {
    clearTimeout(this.timer);
    for (const s of this.queued) { try { s.stop(); } catch { /* already ended */ } }
    this.queued = [];
  }

  pump() {
    if (!this.playing) return;
    const sr = this.sampleRate;
    const now = this.ctx.currentTime;
    // if the page was busy and the queue ran dry, carry on from now rather than in the past
    if (this.base + this.core.rendered / sr < now) this.base = now + 0.02 - this.core.rendered / sr;
    while (this.base + this.core.rendered / sr < now + AHEAD) {
      const at = this.base + this.core.rendered / sr;
      const { part, clicks } = this.core.render(BLOCK);
      for (const [data, gain] of [[part, this.partGain], [clicks, this.clickGain]]) {
        const buf = this.ctx.createBuffer(1, BLOCK, sr);
        buf.copyToChannel(data, 0);
        const src = this.ctx.createBufferSource();
        src.buffer = buf;
        src.connect(gain);
        src.start(at);
        src.onended = () => { const i = this.queued.indexOf(src); if (i >= 0) this.queued.splice(i, 1); };
        this.queued.push(src);
      }
    }
    if (!this.core.playing) { // reached the end without looping
      this.timer = setTimeout(() => { this.pausedAt = 0; this.core.seek(0); this.playing = false; this.onstate?.(); }, AHEAD * 1000);
      return;
    }
    this.timer = setTimeout(() => this.pump(), 40);
  }
}
