// SPDX-License-Identifier: Apache-2.0
// Tab layout: turns one track into drawing primitives plus cursor geometry.
// Colours are not decided here, only roles; a Theme maps roles to colours.
#pragma once

#include "model.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace tab {

enum class Role : uint8_t {
	Staff, BarLine, BarThick, RepeatDot, Note, NoteSoft, Clef, StringName, TimeSig, BarNumber, Marker,
	Chord, BeatText, SmallText, Technique, TechniqueFill, Dashed, Stem, Beam, Rest, Arrow
};

struct PathOp {
	char op; // M L Q C Z
	float x[3], y[3];
};

struct Prim {
	enum Type : uint8_t { Line, Rect, Text, Path, Circle } type = Line;
	Role role = Role::Staff;
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // line ends / rect corners / text anchor (x0,y0) / circle centre + radius (x1)
	float stroke = 1;
	float size = 12; // font size
	uint8_t align = 1; // text: 0 start, 1 centre, 2 end
	bool bold = false, italic = false;
	int string = -1;  // note colour by string
	int beatKey = -1; // bar * 4096 + beat for notes of voice 1
	std::string text;
	std::vector<PathOp> path;
};

struct BarGeo {
	int index = 0, sys = 0;
	float x0 = 0, x1 = 0;
};
struct SysGeo {
	float y = 0, top = 0, bottom = 0, height = 0, x0 = 0, x1 = 0;
};

struct Layout {
	float width = 0, height = 0;
	float scale = 1;
	std::vector<Prim> prims;
	std::vector<BarGeo> bars;   // by bar index
	std::vector<SysGeo> systems;
	std::unordered_map<int, float> beats; // beatKey -> x
};

struct LayoutOptions {
	float width = 900;
	bool horizontal = false;
	float scale = 1;
	bool rhythm = true, barNumbers = true, chords = true, techniques = true;
	int labels = 0; // 0 TAB, 1 string names, 2 none
	float spacing = 1;
};

Layout layoutTrack(const Score &score, int track, const LayoutOptions &options);

std::string noteLabel(const Note &n);

} // namespace tab
