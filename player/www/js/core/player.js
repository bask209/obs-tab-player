// SPDX-License-Identifier: Apache-2.0
// Transport: schedules timeline notes on the audio clock with a small look-ahead,
// handles speed, looping, count-in and metronome, and reports the song position.

import { countInClicks, loopRange } from './timeline.js';

const LOOKAHEAD = 0.35; // seconds scheduled ahead of the audio clock
const INTERVAL = 90; // ms between scheduler wake-ups

export class Player {
  constructor(synth) {
    this.synth = synth;
    this.score = null;
    this.tl = null;
    this.speed = 1;
    this.loop = true;
    this.loopBars = [0, 0];
    this.countIn = true;
    this.metronome = false;
    this.playing = false;
    this.pos = 0; // song seconds while paused
    this.timer = null;
    this.onstate = () => {};
    this.onend = () => {};
  }

  load(score, tl) {
    this.stop();
    this.score = score;
    this.tl = tl;
    this.synth.prepare(tl.notes);
    this.synth.trim(tl.notes);
    this.synth.sleepSoon(0);
    this.pos = 0;
  }

  get range() {
    if (!this.tl) return [0, 0];
    return loopRange(this.tl, this.loopBars[0], this.loopBars[1]);
  }

  async play() {
    if (!this.tl || this.playing) return;
    await this.synth.resume();
    const ctx = this.synth.ctx;
    const [a, b] = this.range;
    if (this.pos < a || this.pos >= b - 0.01) this.pos = a;
    let start = ctx.currentTime + 0.06;
    this.countInUntil = 0;
    if (this.countIn) {
      const ci = countInClicks(this.score, this.tl, this.pos);
      for (const c of ci.clicks) this.synth.click(start + c.t / this.speed, c.down);
      this.countInFrom = start;
      start += ci.dur / this.speed;
      this.countInUntil = start;
      this.countInBeats = ci.clicks.length;
    }
    this.seg = { ctx: start, song: this.pos, end: b };
    this.next = this.pos;
    this.nextClick = this.pos;
    this.playing = true;
    this.tick();
    this.timer = setInterval(() => this.tick(), INTERVAL);
    this.onstate();
  }

  pause() {
    if (!this.playing) return;
    this.pos = this.position();
    this.halt();
  }

  stop() {
    this.halt();
    this.pos = this.tl ? this.range[0] : 0;
    this.onstate();
  }

  halt() {
    const wasPlaying = this.playing;
    this.playing = false;
    clearInterval(this.timer);
    this.timer = null;
    this.synth.silence();
    this.synth.sleepSoon();
    if (wasPlaying) this.onstate();
  }

  seek(songTime) {
    const playing = this.playing;
    if (playing) this.halt();
    this.pos = Math.max(0, Math.min(songTime, this.tl ? this.tl.duration : 0));
    if (playing) { const ci = this.countIn; this.countIn = false; this.play(); this.countIn = ci; }
    else this.onstate();
  }

  setSpeed(speed) {
    if (speed === this.speed) return;
    if (this.playing) {
      const ctx = this.synth.ctx;
      const now = ctx.currentTime;
      if (now < this.countInUntil) {
        // still counting in: simplest is to restart the count-in at the new speed
        this.speed = speed;
        this.halt();
        this.play();
        return;
      }
      const song = this.position();
      this.synth.cancelFuture(now + 0.02);
      this.speed = speed;
      this.seg = { ctx: now + 0.02, song, end: this.seg.end };
      this.next = song;
      this.nextClick = song;
    } else {
      this.speed = speed;
    }
  }

  setLoop(enabled, fromBar = 0, toBar = 0) {
    const changed = this.loopBars[0] !== fromBar || this.loopBars[1] !== toBar;
    this.loop = enabled;
    this.loopBars = [fromBar, toBar];
    if (changed && this.playing) this.seek(this.range[0]);
    else if (this.seg) this.seg.end = this.range[1];
  }

  // Song position (seconds at speed 1) for an audio-clock time.
  position(at) {
    if (!this.playing) return this.pos;
    const t = at ?? this.synth.ctx.currentTime;
    let seg = this.seg;
    if (this.prevSeg && t < seg.ctx) seg = this.prevSeg;
    const p = seg.song + Math.max(0, t - seg.ctx) * this.speed;
    return Math.min(p, seg.end);
  }

  // Count-in progress: 0 when not counting, else the beat number (1-based).
  countInBeat(at) {
    const t = at ?? (this.synth.ctx ? this.synth.ctx.currentTime : 0);
    if (!this.playing || t >= this.countInUntil || !this.countInBeats) return 0;
    const per = (this.countInUntil - this.countInFrom) / this.countInBeats;
    return Math.min(this.countInBeats, Math.floor((t - this.countInFrom) / per) + 1);
  }

  // Seconds until the next count-in click (or the end of the count-in).
  countInWait(at) {
    const t = at ?? this.synth.ctx.currentTime;
    const per = (this.countInUntil - this.countInFrom) / this.countInBeats;
    const k = Math.floor((t - this.countInFrom) / per) + 1;
    return Math.max(0.005, Math.min(this.countInUntil, this.countInFrom + k * per) - t);
  }

  tick() {
    if (!this.playing) return;
    const ctx = this.synth.ctx;
    const horizon = ctx.currentTime + LOOKAHEAD;
    const notes = this.tl.notes;
    const clicks = this.tl.clicks;
    for (let guard = 0; guard < 8; guard++) {
      const seg = this.seg;
      const segEndCtx = seg.ctx + (seg.end - seg.song) / this.speed;
      const limitCtx = Math.min(horizon, segEndCtx);
      const limitSong = seg.song + (limitCtx - seg.ctx) * this.speed;
      // notes in [next, limitSong)
      let i = lowerBound(notes, this.next);
      for (; i < notes.length && notes[i].t < limitSong; i++) {
        const n = notes[i];
        if (n.t >= seg.end) break;
        const when = seg.ctx + (n.t - seg.song) / this.speed;
        const clipped = n.t + n.dur > seg.end ? { ...n, dur: seg.end - n.t } : n;
        this.synth.note(clipped, Math.max(when, ctx.currentTime), this.speed);
      }
      if (this.metronome) {
        let k = lowerBound(clicks, this.nextClick);
        for (; k < clicks.length && clicks[k].t < limitSong && clicks[k].t < seg.end; k++) {
          this.synth.click(seg.ctx + (clicks[k].t - seg.song) / this.speed, clicks[k].down);
        }
      }
      this.next = limitSong;
      this.nextClick = limitSong;
      if (horizon < segEndCtx) break;
      // reached the end of the range
      if (this.loop) {
        const [a, b] = this.range;
        this.prevSeg = seg;
        this.seg = { ctx: segEndCtx, song: a, end: b };
        this.next = a;
        this.nextClick = a;
      } else {
        const endAt = segEndCtx;
        clearInterval(this.timer);
        this.timer = setTimeout(() => {
          this.playing = false;
          this.timer = null;
          this.synth.sleepSoon();
          this.pos = this.range[0];
          this.onstate();
          this.onend();
        }, Math.max(0, (endAt - ctx.currentTime) * 1000 + 50));
        this.playing = true;
        this.seg = seg;
        this.next = Infinity;
        this.nextClick = Infinity;
        this.ending = true;
        return;
      }
    }
  }
}

function lowerBound(arr, t) {
  let lo = 0;
  let hi = arr.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (arr[mid].t < t - 1e-9) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}
