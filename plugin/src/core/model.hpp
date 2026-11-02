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
// Swing ("triplet feel") of a bar: how pairs of 8ths (or 16ths) are played.
enum class Feel : uint8_t { None, Triplet8th, Triplet16th, Dotted8th, Dotted16th, Scottish8th, Scottish16th };

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
	bool letRing = false, palmMute = false, staccato = false, vibrato = false, wideVibrato = false;
	int accent = 0;   // 0 none, 1 accent, 2 heavy
	int dynamic = -1; // 1 (ppp) .. 8 (fff), -1 = default
	Slide slide = Slide::None;
	SlideIn slideIn = SlideIn::None;
	Harmonic harmonic = Harmonic::None;
	float harmonicFret = 0; // natural: where the string is touched (e.g. 3.2); others: frets above the played fret
	int trill = -1;         // trill with this fret, -1 = none
	int trillSpeed = 16;    // trill note value (16, 32, 64)
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
	uint8_t technique = 0; // 1 tap, 2 slap, 3 pop
	uint8_t grace = 0;     // Guitar Pro 6-8 grace beat: 1 before the beat, 2 on the beat (folded into the next beat)
	double tempo = 0;   // tempo change at this beat (GP3-5 mix table)
	std::vector<BendPoint> whammy; // tremolo bar: pitch of every note of the beat over its duration
	uint8_t beam = 0;   // beaming to the previous beat: 0 automatic, 1 break, 2 join, 3 break secondary beams
};

using Voice = std::vector<Beat>;

struct Bar {
	std::vector<Voice> voices;
};

struct TempoChange {
	double ratio = 0; // position in the bar, 0..1
	double bpm = 120;
};

// Musical directions: signs a bar carries, and the jump at its end.
enum DirectionTarget : uint8_t { kCoda = 1, kDoubleCoda = 2, kSegno = 4, kSegnoSegno = 8, kFine = 16 };
enum class Jump : uint8_t {
	None, DaCapo, DaCapoAlCoda, DaCapoAlDoubleCoda, DaCapoAlFine, DaSegno, DaSegnoAlCoda, DaSegnoAlDoubleCoda, DaSegnoAlFine,
	DaSegnoSegno, DaSegnoSegnoAlCoda, DaSegnoSegnoAlDoubleCoda, DaSegnoSegnoAlFine, DaCoda, DaDoubleCoda
};

struct Fermata {
	double ratio = 0;    // position in the bar, 0..1
	double length = 0.5; // extra time, as a share of the beat it holds
};

struct MasterBar {
	int num = 4, den = 4;
	bool repeatStart = false;
	int repeatPlays = 0;    // total plays at the repeat close, 0 = no repeat
	uint32_t alternate = 0; // bit n = ending n+1
	std::string marker;
	bool doubleBar = false;
	Feel feel = Feel::None;
	std::vector<TempoChange> tempoChanges;
	uint8_t targets = 0; // DirectionTarget bits
	Jump jump = Jump::None;
	std::vector<Fermata> fermatas;
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
// Semitones above the open string at which a harmonic touched at this fret sounds.
int naturalHarmonicOffset(double fret);

// Computes beat ticks/starts, resolves tied frets, removes empty voices.
void finishScore(Score &score);

// Reads any supported format. Throws std::runtime_error on bad input.
Score loadScore(const uint8_t *data, size_t size);

} // namespace tab
