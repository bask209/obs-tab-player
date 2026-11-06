// SPDX-License-Identifier: Apache-2.0
#include "synth.hpp"

#include <algorithm>
#include <cmath>

namespace tab {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRingSeconds = 3.2;

struct Biquad {
	double b0, b1, b2, a1, a2;
	void run(std::vector<float> &d) const
	{
		double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
		for (auto &s : d) {
			double x = s;
			double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
			x2 = x1, x1 = x, y2 = y1, y1 = y;
			s = float(y);
		}
	}
};
Biquad norm(double b0, double b1, double b2, double a0, double a1, double a2)
{
	return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}
Biquad lowpass(double f, double q, double sr)
{
	double w = 2 * kPi * f / sr, c = std::cos(w), al = std::sin(w) / (2 * q);
	return norm((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}
Biquad peaking(double f, double q, double db, double sr)
{
	double A = std::pow(10, db / 40), w = 2 * kPi * f / sr, c = std::cos(w), al = std::sin(w) / (2 * q);
	return norm(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}

int kindIndex(NoteKind k) { return k == NoteKind::Muted ? 1 : k == NoteKind::Dead ? 2 : k == NoteKind::Harmonic ? 3 : 0; }

} // namespace

Synth::Synth(int sampleRate) : sr_(sampleRate)
{
	auto click = [&](double freq, double len, Buffer &b) {
		size_t n = size_t(sr_ * len);
		b.data.resize(n);
		for (size_t i = 0; i < n; i++) {
			double t = double(i) / sr_;
			b.data[i] = float(std::sin(2 * kPi * freq * t) * std::exp(-t * 90) * 0.9);
		}
	};
	click(1760, 0.035, clickHi_);
	click(1175, 0.03, clickLo_);
	segments_.push_back({0, 0, 0});
}

void Synth::renderBank(int midi, NoteKind kind, Tone tone, Buffer &out) const
{
	const double sr = sr_;
	const double f = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
	const bool dead = kind == NoteKind::Dead, muted = kind == NoteKind::Muted, harmonic = kind == NoteKind::Harmonic;
	const double seconds = dead ? 0.09 : muted ? 0.45 : kRingSeconds;
	const size_t len = size_t(sr * seconds);
	out.data.assign(len, 0.f);

	// Loop delay = N - 0.5 (average of the two oldest samples) + allpass delay in [0.15, 1.15).
	const double period = sr / f;
	const int N = std::max(2, int(std::floor(period + 0.5 - 0.15)));
	const double frac = period + 0.5 - N;
	const double C = (1 - frac) / (1 + frac);
	const double t60 = muted ? 0.25 : dead ? 0.04 : std::max(0.9, 4.2 - (midi - 40) * 0.07);
	const double loss = std::pow(10.0, -3.0 / (f * t60));
	const double bright = muted ? 0.25 : harmonic ? 0.2 : 0.5;

	std::vector<double> line(size_t(N) + 2);
	uint32_t seed = uint32_t(midi * 9301 + 49297);
	double lp = 0;
	for (size_t i = 0; i < line.size(); i++) {
		seed = seed * 1664525u + 1013904223u;
		double x = harmonic ? std::sin(2 * kPi * double(i) * 2 / N) : (double(seed >> 8) / double(1 << 23)) - 1.0;
		lp += (x - lp) * (muted ? 0.35 : 0.7);
		line[i] = lp;
	}
	int pick = std::max(1, int(std::lround(N * 0.13)));
	for (int i = N + 1; i >= pick; i--)
		line[size_t(i)] -= line[size_t(i - pick)] * 0.9;
	double mean = 0, peak = 0;
	for (double v : line)
		mean += v;
	mean /= double(line.size());
	for (double &v : line) {
		v -= mean;
		peak = std::max(peak, std::abs(v));
	}
	const double gain = 0.5 / (peak > 0 ? peak : 1);

	size_t idx = 0;
	double apX = 0, apY = 0;
	for (size_t i = 0; i < len; i++) {
		double cur = line[idx], next = line[(idx + 1) % size_t(N)];
		double avg = (1 - bright) * cur + bright * next;
		double y = C * avg + apX - C * apY;
		apX = avg;
		apY = y;
		line[idx] = y * loss;
		idx = (idx + 1) % size_t(N);
		out.data[i] = float(cur * gain);
	}
	// tone, baked in
	peaking(220, 0.9, 3, sr).run(out.data);
	if (tone == Tone::Crunch)
		for (auto &s : out.data)
			s = float(std::tanh(s * 4) * 0.7);
	lowpass(tone == Tone::Warm ? 3200 : tone == Tone::Crunch ? 4200 : 7500, 0.707, sr).run(out.data);
	size_t fade = std::min(len, size_t(sr * 0.05));
	for (size_t i = 0; i < fade; i++)
		out.data[len - 1 - i] *= float(i) / float(fade);
}

const Synth::Buffer *Synth::bank(int midi, NoteKind kind)
{
	midi = std::clamp(midi, 16, 120);
	int key = midi * 4 + kindIndex(kind);
	auto it = bank_.find(key);
	if (it != bank_.end())
		return it->second.get();
	auto b = std::make_unique<Buffer>();
	renderBank(midi, kind, tone_, *b);
	auto *raw = b.get();
	bank_[key] = std::move(b);
	return raw;
}

void Synth::load(const Timeline &tl, const CountIn *ci)
{
	// Render the sounds this piece needs first, without the lock: on a slow machine it takes a
	// second, and the video thread asks for the position (under this lock) every frame.
	Tone tone;
	std::unordered_map<int, bool> have;
	{
		std::lock_guard<std::mutex> lock(mu_);
		tone = tone_;
		for (auto &kv : bank_)
			have[kv.first] = true;
	}
	std::unordered_map<int, std::unique_ptr<Buffer>> fresh;
	for (auto &n : tl.notes) {
		int midi = std::clamp(n.midi, 16, 120);
		int key = midi * 4 + kindIndex(n.kind);
		if (have.count(key) || fresh.count(key))
			continue;
		auto b = std::make_unique<Buffer>();
		renderBank(midi, n.kind, tone, *b);
		fresh[key] = std::move(b);
	}
	std::lock_guard<std::mutex> lock(mu_);
	if (tone != tone_) // the tone changed meanwhile: these are stale
		fresh.clear();
	for (auto &v : voices_)
		v.active = false;
	notes_ = tl.notes;
	clicks_ = tl.clicks;
	duration_ = tl.duration;
	if (ci)
		countIn_ = *ci;
	loopA_ = 0;
	loopB_ = duration_;
	playing_ = false;
	pos_ = 0;
	countInEnd_ = -1;
	// keep only the buffers this piece needs, render missing ones now (never on the audio thread)
	std::unordered_map<int, std::unique_ptr<Buffer>> keep;
	for (auto &n : notes_) {
		int midi = std::clamp(n.midi, 16, 120);
		int key = midi * 4 + kindIndex(n.kind);
		if (keep.count(key))
			continue;
		auto f = fresh.find(key);
		auto it = bank_.find(key);
		if (f != fresh.end())
			keep[key] = std::move(f->second);
		else if (it != bank_.end())
			keep[key] = std::move(it->second);
		else {
			auto b = std::make_unique<Buffer>();
			renderBank(midi, n.kind, tone_, *b);
			keep[key] = std::move(b);
		}
	}
	bank_ = std::move(keep);
	pushSegment(sample_, 0, 0);
}

void Synth::setCountIn(const CountIn &ci)
{
	std::lock_guard<std::mutex> lock(mu_);
	countIn_ = ci;
}

void Synth::setTone(Tone tone)
{
	auto kindOf = [](int key) { return key % 4 == 1 ? NoteKind::Muted : key % 4 == 2 ? NoteKind::Dead : key % 4 == 3 ? NoteKind::Harmonic : NoteKind::Pluck; };
	// render the new sounds without the lock, then swap them in
	std::vector<int> keys;
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (tone == tone_)
			return;
		for (auto &kv : bank_)
			keys.push_back(kv.first);
	}
	std::unordered_map<int, std::unique_ptr<Buffer>> fresh;
	for (int key : keys) {
		auto b = std::make_unique<Buffer>();
		renderBank(key / 4, kindOf(key), tone, *b);
		fresh[key] = std::move(b);
	}
	std::lock_guard<std::mutex> lock(mu_);
	tone_ = tone;
	for (auto &v : voices_)
		v.active = false;
	for (auto &kv : bank_) {
		auto f = fresh.find(kv.first);
		if (f != fresh.end())
			kv.second = std::move(f->second);
		else // added meanwhile
			renderBank(kv.first / 4, kindOf(kv.first), tone_, *kv.second);
	}
}

void Synth::pushSegment(int64_t sample, double pos, double rate, double countInStart, double per, int beats)
{
	// a new segment replaces any that would start at or after it (e.g. a pause during a count-in)
	while (!segments_.empty() && segments_.back().sample >= sample)
		segments_.pop_back();
	segments_.push_back({sample, pos, rate, countInStart, per, beats});
	if (segments_.size() > 64)
		segments_.erase(segments_.begin(), segments_.begin() + 32);
}

size_t Synth::lowerBound(double t) const
{
	return size_t(std::lower_bound(notes_.begin(), notes_.end(), t - 1e-9, [](const NoteEvent &n, double v) { return n.t < v; }) - notes_.begin());
}

void Synth::play(double from, double speed, bool countIn, bool loop)
{
	std::lock_guard<std::mutex> lock(mu_);
	if (notes_.empty() && duration_ <= 0)
		return;
	speed_ = std::clamp(speed, 0.1, 4.0);
	loop_ = loop;
	if (from < loopA_ || from >= loopB_ - 0.01)
		from = loopA_;
	pos_ = from;
	nextNote_ = lowerBound(from);
	nextClick_ = size_t(std::lower_bound(clicks_.begin(), clicks_.end(), from - 1e-9, [](const Click &c, double v) { return c.t < v; }) - clicks_.begin());
	playing_ = true;
	int64_t start = sample_ + int64_t(sr_ * 0.03);
	if (countIn && !countIn_.clicks.empty()) {
		double per = countIn_.dur / countIn_.clicks.size() / speed_ * sr_;
		for (auto &c : countIn_.clicks)
			startClick(c.down, start + int64_t(c.t / speed_ * sr_));
		pushSegment(sample_, from, 0, double(start), per, int(countIn_.clicks.size()));
		start += int64_t(countIn_.dur / speed_ * sr_);
	} else {
		pushSegment(sample_, from, 0);
	}
	countInEnd_ = start;
	pushSegment(start, from, speed_ / sr_);
}

void Synth::pause()
{
	std::lock_guard<std::mutex> lock(mu_);
	if (!playing_)
		return;
	playing_ = false;
	for (auto &v : voices_) {
		if (v.active && v.startSample > sample_)
			v.active = false; // scheduled but not started yet (e.g. count-in clicks)
		else if (v.active && !v.release)
			v.release = std::exp(-1.0f / (0.02f * sr_));
	}
	pushSegment(sample_, pos_, 0);
}

void Synth::stop()
{
	pause();
	std::lock_guard<std::mutex> lock(mu_);
	pos_ = loopA_;
	pushSegment(sample_, pos_, 0);
}

void Synth::seek(double t)
{
	bool was;
	{
		std::lock_guard<std::mutex> lock(mu_);
		was = playing_;
	}
	if (was) {
		pause();
		play(t, speed_, false, loop_);
	} else {
		std::lock_guard<std::mutex> lock(mu_);
		pos_ = std::clamp(t, 0.0, duration_);
		pushSegment(sample_, pos_, 0);
	}
}

void Synth::setSpeed(double speed)
{
	std::lock_guard<std::mutex> lock(mu_);
	speed_ = std::clamp(speed, 0.1, 4.0);
	if (playing_ && sample_ >= countInEnd_)
		pushSegment(sample_, pos_, speed_ / sr_);
	else if (playing_ && !segments_.empty() && segments_.back().sample == countInEnd_)
		segments_.back().rate = speed_ / sr_; // during a count-in: play on at the new speed
}

void Synth::setLoop(bool enabled, double a, double b)
{
	std::lock_guard<std::mutex> lock(mu_);
	loop_ = enabled;
	loopA_ = std::clamp(a, 0.0, duration_);
	loopB_ = b > loopA_ ? std::min(b, duration_) : duration_;
}

void Synth::setLoopCountIn(const CountIn &ci) { std::lock_guard<std::mutex> lock(mu_); loopCountIn_ = ci; }
void Synth::setLoopGap(int mode) { std::lock_guard<std::mutex> lock(mu_); loopGap_ = std::clamp(mode, 0, 2); }
void Synth::setMetronome(bool on) { std::lock_guard<std::mutex> lock(mu_); metronome_ = on; }
void Synth::setVolume(float v) { std::lock_guard<std::mutex> lock(mu_); volume_ = v; }
void Synth::setClickVolume(float v) { std::lock_guard<std::mutex> lock(mu_); clickVolume_ = v; }
double Synth::duration() const { std::lock_guard<std::mutex> lock(mu_); return duration_; }
int64_t Synth::samplesRendered() const { std::lock_guard<std::mutex> lock(mu_); return sample_; }

bool Synth::idle() const
{
	std::lock_guard<std::mutex> lock(mu_);
	if (playing_)
		return false;
	for (auto &v : voices_)
		if (v.active)
			return false;
	return true;
}

double Synth::positionAt(int64_t sample) const
{
	std::lock_guard<std::mutex> lock(mu_);
	const Segment *seg = &segments_.front();
	for (auto &s : segments_)
		if (s.sample <= sample)
			seg = &s;
	double p = seg->pos + double(sample - seg->sample) * seg->rate;
	if (seg->rate > 0 && loop_ && p >= loopB_)
		p = loopA_ + std::fmod(p - loopA_, std::max(1e-6, loopB_ - loopA_));
	return std::min(p, duration_);
}

double Synth::positionAndCountIn(int64_t sample, int *countIn) const
{
	std::lock_guard<std::mutex> lock(mu_);
	const Segment *seg = &segments_.front();
	for (auto &s : segments_)
		if (s.sample <= sample)
			seg = &s;
	if (countIn) {
		*countIn = 0;
		if (seg->countInStart >= 0 && seg->countInPer > 0 && sample >= seg->countInStart) {
			int k = int((double(sample) - seg->countInStart) / seg->countInPer) + 1;
			*countIn = k <= seg->countInBeats ? k : 0;
		}
	}
	double p = seg->pos + double(sample - seg->sample) * seg->rate;
	if (seg->rate > 0 && loop_ && p >= loopB_)
		p = loopA_ + std::fmod(p - loopA_, std::max(1e-6, loopB_ - loopA_));
	return std::min(p, duration_);
}

int Synth::countInBeatAt(int64_t sample) const
{
	std::lock_guard<std::mutex> lock(mu_);
	const Segment *seg = &segments_.front();
	for (auto &s : segments_)
		if (s.sample <= sample)
			seg = &s;
	if (seg->countInStart < 0 || seg->countInPer <= 0 || sample < seg->countInStart)
		return 0;
	int k = int((double(sample) - seg->countInStart) / seg->countInPer) + 1;
	return k <= seg->countInBeats ? k : 0;
}

void Synth::startClick(bool down, int64_t at)
{
	for (auto &v : voices_)
		if (!v.active) {
			v = Voice();
			v.buf = down ? &clickHi_ : &clickLo_;
			v.gain = -1; // marks a click (uses click volume)
			v.startSample = at;
			v.endSample = INT64_MAX;
			v.active = true;
			return;
		}
}

void Synth::startNote(const NoteEvent &ev, int64_t at)
{
	const Buffer *buf = bank(ev.midi, ev.kind);
	int key = ev.track * 16 + ev.string;
	// one sound per string: fade the previous one quickly
	for (auto &v : voices_)
		if (v.active && v.key == key && v.startSample < at) {
			v.endSample = std::min(v.endSample, at);
			v.release = std::exp(-1.0f / (0.006f * sr_));
		}
	Voice *slot = nullptr;
	for (auto &v : voices_)
		if (!v.active) {
			slot = &v;
			break;
		}
	if (!slot) { // steal the quietest
		slot = &voices_[0];
		for (auto &v : voices_)
			if (v.env * std::abs(v.gain) < slot->env * std::abs(slot->gain))
				slot = &v;
	}
	Voice v;
	v.buf = buf;
	v.key = key;
	v.gain = ev.vel * (ev.legato ? 0.55f : 1.f) * (ev.kind == NoteKind::Dead ? 0.7f : 1.f);
	v.startSample = at;
	double dur = ev.dur / speed_;
	v.endSample = at + int64_t(dur * sr_);
	v.vibrato = ev.vibrato && dur > 0.15;
	// pitch curve in (seconds since start, semitones)
	if (ev.slideIn != 0) {
		v.rate.push_back({0, ev.slideIn});
		v.rate.push_back({std::min(0.08, dur / 2), 0});
	}
	if (!ev.bend.empty())
		for (auto &p : ev.bend)
			v.rate.push_back({p.t / speed_, p.semis});
	if (ev.slideNext && ev.slideTarget >= 0) {
		double tail = std::min(0.12, dur * 0.4);
		v.rate.push_back({dur - tail, 0});
		v.rate.push_back({dur, double(ev.slideTarget - ev.midi)});
	} else if (ev.slideTarget >= 0) {
		v.rate.push_back({0, 0});
		v.rate.push_back({dur, double(ev.slideTarget - ev.midi)});
	}
	if (ev.slideOut != 0) {
		double tail = std::min(0.15, dur * 0.5);
		v.rate.push_back({dur - tail, 0});
		v.rate.push_back({dur, ev.slideOut});
	}
	std::sort(v.rate.begin(), v.rate.end(), [](auto &a, auto &b) { return a.first < b.first; });
	v.active = true;
	*slot = std::move(v);
}

void Synth::schedule(int64_t from, int64_t to)
{
	// Schedule notes whose song time falls in [pos at from, pos at to).
	if (!playing_ || to <= countInEnd_)
		return;
	int64_t s = std::max(from, countInEnd_);
	while (s < to) {
		double rate = speed_ / sr_;
		double endPos = pos_ + double(to - s) * rate;
		double limit = loop_ ? loopB_ : duration_;
		double segEnd = std::min(endPos, limit);
		while (nextNote_ < notes_.size() && notes_[nextNote_].t < segEnd) {
			auto &n = notes_[nextNote_++];
			if (n.t < pos_ - 1e-6)
				continue;
			int64_t at = s + int64_t((n.t - pos_) / rate);
			NoteEvent ev = n;
			if (ev.t + ev.dur > limit)
				ev.dur = std::max(0.01, limit - ev.t);
			startNote(ev, at);
		}
		if (metronome_)
			while (nextClick_ < clicks_.size() && clicks_[nextClick_].t < segEnd) {
				auto &c = clicks_[nextClick_++];
				if (c.t >= pos_ - 1e-6)
					startClick(c.down, s + int64_t((c.t - pos_) / rate));
			}
		else
			while (nextClick_ < clicks_.size() && clicks_[nextClick_].t < segEnd)
				nextClick_++;
		if (endPos < limit) {
			pos_ = endPos;
			return;
		}
		// reached the end of the range
		int64_t at = s + int64_t((limit - pos_) / rate);
		if (loop_) {
			pos_ = loopA_;
			nextNote_ = lowerBound(loopA_);
			nextClick_ = size_t(std::lower_bound(clicks_.begin(), clicks_.end(), loopA_ - 1e-9, [](const Click &c, double v) { return c.t < v; }) - clicks_.begin());
			// a bar between repeats: silent or clicked, counted on screen; the cursor waits at the loop start
			const CountIn &ci = loopCountIn_.clicks.empty() ? countIn_ : loopCountIn_;
			const int64_t gap = loopGap_ > 0 && !ci.clicks.empty() ? int64_t(ci.dur / speed_ * sr_) : 0;
			if (gap > 0) {
				const double per = ci.dur / double(ci.clicks.size()) / speed_ * sr_;
				if (loopGap_ == 2)
					for (auto &c : ci.clicks)
						startClick(c.down, at + int64_t(c.t / speed_ * sr_));
				pushSegment(at, loopA_, 0, double(at), per, int(ci.clicks.size()));
				countInEnd_ = at + gap;
				pushSegment(countInEnd_, loopA_, rate);
				s = std::max(countInEnd_, s + 1);
			} else {
				pushSegment(at, loopA_, rate);
				s = std::max(at, s + 1);
			}
		} else {
			playing_ = false;
			pos_ = loopA_;
			pushSegment(at, loopA_, 0);
			return;
		}
	}
}

void Synth::render(float *out, size_t frames, float *clickOut)
{
	std::lock_guard<std::mutex> lock(mu_);
	std::fill(out, out + frames, 0.f);
	if (clickOut)
		std::fill(clickOut, clickOut + frames, 0.f);
	const int64_t start = sample_, end = sample_ + int64_t(frames);
	schedule(start, end);
	const double twoPi = 2 * kPi;
	for (auto &v : voices_) {
		if (!v.active)
			continue;
		const auto &data = v.buf->data;
		const bool click = v.gain < 0;
		const float g = click ? clickVolume_ : v.gain * volume_;
		float *dst = click && clickOut ? clickOut : out;
		const bool curve = !v.rate.empty() || v.vibrato;
		double rate = 1;
		for (int64_t s = std::max(start, v.startSample); s < end; s++) {
			if (s >= v.endSample && v.release == 0)
				v.release = std::exp(-1.0f / ((click ? 0.01f : 0.03f) * sr_));
			if (v.release > 0) {
				v.env *= v.release;
				if (v.env < 1e-4f) {
					v.active = false;
					break;
				}
			}
			size_t i = size_t(v.pos);
			if (i + 1 >= data.size()) {
				v.active = false;
				break;
			}
			float frac = float(v.pos - double(i));
			dst[size_t(s - start)] += (data[i] + (data[i + 1] - data[i]) * frac) * g * v.env;
			// playback rate: pitch curve and vibrato, at control rate (every 16 samples)
			if (curve && ((s - v.startSample) & 15) == 0) {
				double t = double(s - v.startSample) / sr_;
				double semis = 0;
				if (!v.rate.empty()) {
					if (t <= v.rate.front().first)
						semis = t < v.rate.front().first && v.rate.front().first > 0 ? 0 : v.rate.front().second;
					else if (t >= v.rate.back().first)
						semis = v.rate.back().second;
					else
						for (size_t k = 1; k < v.rate.size(); k++)
							if (t < v.rate[k].first) {
								auto &a = v.rate[k - 1], &b = v.rate[k];
								double f = (t - a.first) / std::max(1e-9, b.first - a.first);
								semis = a.second + (b.second - a.second) * f;
								break;
							}
				}
				rate = std::pow(2.0, std::clamp(semis, -48.0, 48.0) / 12.0);
				if (v.vibrato && t > 0.08)
					rate *= 1 + 0.012 * std::sin(twoPi * 5.5 * t);
				if (!std::isfinite(rate))
					rate = 1;
			}
			v.pos += rate;
		}
	}
	// Soft limiter: transparent below 0.85, never above 1.0, and never NaN.
	auto limit = [frames](float *buf) {
		for (size_t i = 0; i < frames; i++) {
			float x = buf[i];
			if (!std::isfinite(x))
				x = 0;
			float a = std::abs(x);
			if (a > 0.85f)
				x = std::copysign(0.85f + 0.15f * std::tanh((a - 0.85f) / 0.15f), x);
			buf[i] = x;
		}
	};
	limit(out);
	if (clickOut)
		limit(clickOut);
	sample_ = end;
}

} // namespace tab
