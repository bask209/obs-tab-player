// SPDX-License-Identifier: Apache-2.0
// Score model shared by the readers, timeline, layout and synth.
// Strings are numbered from the top line of the tab: 0 = highest string.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tab {

constexpr int kQuarter = 960;

enum class Slide : uint8_t { None, Shift, Legato, OutDown, OutUp };
enum class SlideIn : uint8_t { None, Below, Above };
enum class Harmonic : uint8_t { None, Natural, Artificial, Tap, Pinch, Semi };
enum class Stroke : uint8_t { None, Up, Down };

struct BendPoint {
	float t = 0;     // 0..1 of the note duration
	float semis = 0; // pitch offset in semitones
};

struct Grace {
	int fret = 0;
	bool slide = false, hammer = false, dead = false, onBeat = false;
};

struct Note {
	int string = 0;
	int fret = 0;
	bool tie = false, dead = false, ghost = false, hammer = false;
	bool letRing = false, palmMute = false, staccato = false, vibrato = false;
	int accent = 0;   // 0 none, 1 accent, 2 heavy
	int dynamic = -1; // 1 (ppp) .. 8 (fff), -1 = default
	Slide slide = Slide::None;
	SlideIn slideIn = SlideIn::None;
	Harmonic harmonic = Harmonic::None;
	int harmonicFret = 0;
	std::vector<BendPoint> bend;
	std::optional<Grace> grace;
};

struct Beat {
	int value = 4; // 1 whole, 2 half, 4 quarter, 8 eighth ...
	int dots = 0;
	int tupletNum = 0, tupletDen = 0;
	int ticks = kQuarter; // duration
	int start = 0;        // position inside the bar
	bool rest = false, fullBar = false;
	std::vector<Note> notes;
	std::string chord, text;
	Stroke pickStroke = Stroke::None;
	Stroke brush = Stroke::None;
	bool vibrato = false, fadeIn = false, letRing = false;
	int tremolo = 0;    // tremolo picking note value (8, 16, 32)
	double tempo = 0;   // tempo change at this beat (GP3-5 mix table)
};

using Voice = std::vector<Beat>;

struct Bar {
	std::vector<Voice> voices;
};

struct TempoChange {
	double ratio = 0; // position in the bar, 0..1
	double bpm = 120;
};

struct MasterBar {
	int num = 4, den = 4;
	bool repeatStart = false;
	int repeatPlays = 0;    // total plays at the repeat close, 0 = no repeat
	uint32_t alternate = 0; // bit n = ending n+1
	std::string marker;
	bool doubleBar = false;
	std::vector<TempoChange> tempoChanges;
};

struct Track {
	std::string name;
	std::vector<int> tuning; // MIDI notes, highest string first
	int capo = 0;
	bool percussion = false;
	std::vector<Bar> bars;
};

struct Score {
	std::string format; // gp3 gp4 gp5 gpx gp7
	std::string title, subTitle, artist, album, words, music, copyright, tab, instructions;
	std::string notices, tempoLabel;
	double tempo = 120;
	std::vector<MasterBar> masterBars;
	std::vector<Track> tracks;
};

int beatTicks(int value, int dots, int tupletNum, int tupletDen);
int barTicks(const MasterBar &mb);
int naturalHarmonicOffset(int fret);

// Computes beat ticks/starts, resolves tied frets, removes empty voices.
void finishScore(Score &score);

// Reads any supported format. Throws std::runtime_error on bad input.
Score loadScore(const uint8_t *data, size_t size);

} // namespace tab
