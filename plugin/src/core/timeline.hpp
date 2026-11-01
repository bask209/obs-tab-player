// SPDX-License-Identifier: Apache-2.0
// Flat, time-sorted playback timeline (seconds at speed 1).
#pragma once

#include "model.hpp"

namespace tab {

enum class NoteKind : uint8_t { Pluck, Muted, Dead, Harmonic };

struct NoteEvent {
	double t = 0, dur = 0;
	int midi = 60;
	float vel = 0.8f;
	int track = 0, string = 0;
	NoteKind kind = NoteKind::Pluck;
	bool legato = false, ring = false, vibrato = false, hammer = false;
	bool slideNext = false, slideLegato = false;
	int slideTarget = -1; // midi to glide to at the end
	float slideOut = 0, slideIn = 0;
	std::vector<BendPoint> bend;
};

struct BarSpan {
	int index = 0;
	double start = 0, end = 0, bpm = 120;
};

struct BeatStop {
	double t = 0, dur = 0;
	int bar = 0, beat = 0, occ = 0;
};

struct Click {
	double t = 0;
	bool down = false;
};

struct Timeline {
	double duration = 0;
	std::vector<int> order;
	std::vector<BarSpan> bars;
	std::vector<NoteEvent> notes;
	std::vector<BeatStop> beats; // cursor stops for the display track, voice 1
	std::vector<Click> clicks;
};

std::vector<int> playOrder(const std::vector<MasterBar> &bars);
Timeline buildTimeline(const Score &score, int displayTrack);

// [start, end) in seconds for bars from..to (1-based, inclusive); 0,0 = whole piece.
std::pair<double, double> loopRange(const Timeline &tl, int fromBar, int toBar);

struct CountIn {
	std::vector<Click> clicks;
	double dur = 0;
};
CountIn countInClicks(const Score &score, const Timeline &tl, double from);

} // namespace tab
