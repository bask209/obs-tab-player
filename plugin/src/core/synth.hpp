// SPDX-License-Identifier: Apache-2.0
// Sample-accurate player + string synthesizer.
//
// Notes are pre-rendered once per pitch (Karplus-Strong with the tone baked in). While
// playing, each voice is a buffer read with an optional pitch-rate curve, so the cost is
// a few multiply-adds per active voice per sample. The song position is tracked per
// rendered sample, so a video renderer can ask for the exact position at any sample time.
#pragma once

#include "timeline.hpp"

#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace tab {

enum class Tone : uint8_t { Clean, Warm, Crunch };

class Synth {
public:
	explicit Synth(int sampleRate = 48000);

	// Loads a timeline and pre-renders every pitch it needs.
	void load(const Timeline &tl, const CountIn *countIn = nullptr);
	void setCountIn(const CountIn &ci);

	void play(double from, double speed, bool countIn, bool loop);
	void pause();
	void stop();
	void seek(double songTime);
	void setSpeed(double speed);
	void setLoop(bool enabled, double start, double end);
	void setMetronome(bool on);
	void setVolume(float v);
	void setClickVolume(float v);
	void setTone(Tone tone);

	bool playing() const;
	double duration() const;
	// Song position at an absolute rendered-sample index (from the start of rendering).
	double positionAt(int64_t sample) const;
	// 0 when not counting in, else the count-in beat (1-based) at that sample.
	int countInBeatAt(int64_t sample) const;
	int64_t samplesRendered() const;
	bool idle() const; // nothing playing and no voice ringing out

	// Mixes the next `frames` samples (mono) into out (overwrites).
	void render(float *out, size_t frames);

private:
	struct Buffer {
		std::vector<float> data;
	};
	struct Voice {
		const Buffer *buf = nullptr;
		double pos = 0;   // read position in samples
		float gain = 0;   // velocity
		float env = 1;    // release envelope
		float release = 0; // per-sample multiplier once releasing (0 = holding)
		int64_t endSample = 0;
		int key = -1; // track*16+string, -1 for clicks
		// pitch curve: playback rate over time (samples since start)
		int64_t startSample = 0;
		double baseRate = 1;
		std::vector<std::pair<double, double>> rate; // (seconds since note start, semitones)
		bool vibrato = false;
		bool active = false;
	};
	struct Segment {
		int64_t sample;  // first sample of the segment
		double pos;      // song position at that sample
		double rate;     // song seconds per sample
		double countInStart = -1; // >=0: inside count-in, sample index where it began
		double countInPer = 0;    // samples per count-in click
		int countInBeats = 0;
	};

	const Buffer *bank(int midi, NoteKind kind);
	void renderBank(int midi, NoteKind kind, Buffer &out) const;
	void startNote(const NoteEvent &ev, int64_t at);
	void startClick(bool down, int64_t at);
	void pushSegment(int64_t sample, double pos, double rate, double countInStart = -1, double per = 0, int beats = 0);
	void schedule(int64_t from, int64_t to);
	size_t lowerBound(double t) const;

	mutable std::mutex mu_;
	int sr_;
	Tone tone_ = Tone::Clean;
	float volume_ = 0.9f, clickVolume_ = 0.8f;
	std::vector<NoteEvent> notes_;
	std::vector<Click> clicks_;
	CountIn countIn_;
	double duration_ = 0;
	std::unordered_map<int, std::unique_ptr<Buffer>> bank_;
	Buffer clickHi_, clickLo_;
	std::array<Voice, 48> voices_{};

	bool playing_ = false;
	bool loop_ = true, metronome_ = false;
	double loopA_ = 0, loopB_ = 0;
	double speed_ = 1;
	double pos_ = 0;           // song position at the next sample to render (while playing)
	size_t nextNote_ = 0, nextClick_ = 0;
	int64_t countInEnd_ = -1;  // sample where count-in ends (playing starts)
	int64_t sample_ = 0;       // samples rendered so far
	std::vector<Segment> segments_;
};

} // namespace tab
