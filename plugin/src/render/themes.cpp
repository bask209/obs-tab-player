// SPDX-License-Identifier: Apache-2.0
// Themes and fonts (the same on every platform), and the WAV writer used by tabtool.
#include "renderer.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace tab {
namespace {

Rgba hex(uint32_t rgb, float a = 1) { return {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a}; }

// Each list names the Windows font first, then the macOS and Linux equivalents; the first one
// installed is used, so the tab looks the same everywhere.
const std::vector<std::string> kSans = {"Segoe UI", "SF Pro Text", "Helvetica Neue", "Noto Sans", "DejaVu Sans"};
const std::vector<std::string> kSerif = {"Georgia", "Charter", "Noto Serif", "DejaVu Serif", "Times New Roman"};
const std::vector<std::string> kMono = {"Cascadia Mono", "Consolas", "SF Mono", "Menlo", "Noto Sans Mono", "DejaVu Sans Mono"};
const std::vector<std::string> kRounded = {"Segoe UI Variable Display", "SF Pro Rounded", "Nunito", "Arial Rounded MT Bold", "Noto Sans", "DejaVu Sans"};

} // namespace

Theme themeByName(const std::string &name)
{
	Theme t;
	t.name = name;
	const std::array<Rgba, 8> bright = {hex(0xf87171), hex(0xfb923c), hex(0xfacc15), hex(0x4ade80), hex(0x38bdf8), hex(0xa78bfa), hex(0xf472b6), hex(0x94a3b8)};
	const std::array<Rgba, 8> deep = {hex(0xdc2626), hex(0xea580c), hex(0xca8a04), hex(0x16a34a), hex(0x0284c7), hex(0x7c3aed), hex(0xdb2777), hex(0x475569)};
	t.fonts = kSans;
	if (name == "light") {
		t.card = hex(0xffffff, 0.95f), t.ink = hex(0x16181d), t.line = hex(0x9aa1ad), t.muted = hex(0x6b7280), t.accent = hex(0x2563eb);
		t.cursor = hex(0x2563eb, 0.9f), t.cursorBar = hex(0xfacc15, 0.18f), t.played = hex(0x2563eb), t.strings = deep;
	} else if (name == "glass") {
		t.card = hex(0x14161e, 0.45f), t.ink = hex(0xffffff), t.line = hex(0xffffff, 0.45f), t.muted = hex(0xffffff, 0.75f), t.accent = hex(0x7dd3fc);
		t.cursor = hex(0x7dd3fc, 0.95f), t.cursorBar = hex(0x7dd3fc, 0.14f), t.played = hex(0x7dd3fc), t.outline = hex(0x000000, 0.55f), t.outlineText = true, t.strings = bright;
	} else if (name == "clear") {
		t.card = hex(0x000000, 0), t.ink = hex(0xffffff), t.line = hex(0xffffff, 0.85f), t.muted = hex(0xffffff, 0.85f), t.accent = hex(0xfde047);
		t.cursor = hex(0xfde047, 0.95f), t.cursorBar = hex(0xfde047, 0.16f), t.played = hex(0xfde047), t.outline = hex(0x000000, 0.85f), t.outlineText = true, t.strings = bright;
	} else if (name == "neon") {
		t.card = hex(0x080614, 0.85f), t.ink = hex(0xe9e7ff), t.line = hex(0xa78bfa, 0.45f), t.muted = hex(0xb8b2ff), t.accent = hex(0xf472b6);
		t.cursor = hex(0x22d3ee), t.cursorBar = hex(0x22d3ee, 0.12f), t.played = hex(0x22d3ee), t.strings = bright;
	} else if (name == "paper") {
		t.card = hex(0xfbf8f1), t.ink = hex(0x2b2620), t.line = hex(0xb3a99a), t.muted = hex(0x7c7262), t.accent = hex(0xb45309);
		t.cursor = hex(0xb45309, 0.85f), t.cursorBar = hex(0xb45309, 0.1f), t.played = hex(0xb45309), t.strings = deep, t.fonts = kSerif;
	} else { // dark (default)
		t.name = "dark";
		t.card = hex(0x0f1117, 0.88f), t.ink = hex(0xf4f6fa), t.line = hex(0x59606e), t.muted = hex(0xa3abb9), t.accent = hex(0x60a5fa);
		t.cursor = hex(0x60a5fa, 0.95f), t.cursorBar = hex(0xfacc15, 0.14f), t.played = hex(0xfbbf24), t.strings = bright;
	}
	return t;
}

std::vector<std::string> themeNames() { return {"dark", "light", "glass", "clear", "neon", "paper"}; }

void applyFont(Theme &theme, const std::string &font)
{
	if (font == "mono") theme.fonts = kMono;
	else if (font == "serif") theme.fonts = kSerif;
	else if (font == "rounded") theme.fonts = kRounded;
	else if (theme.name != "paper") theme.fonts = kSans;
}

bool saveWav(const std::vector<float> &mono, int sampleRate, const std::filesystem::path &path)
{
	std::ofstream out(path, std::ios::binary);
	if (!out)
		return false;
	auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char *>(&v), 4); };
	auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char *>(&v), 2); };
	uint32_t bytes = uint32_t(mono.size() * 2);
	out.write("RIFF", 4); u32(36 + bytes); out.write("WAVEfmt ", 8);
	u32(16); u16(1); u16(1); u32(uint32_t(sampleRate)); u32(uint32_t(sampleRate * 2)); u16(2); u16(16);
	out.write("data", 4); u32(bytes);
	for (float s : mono) {
		int16_t v = int16_t(std::lround(std::clamp(s, -1.f, 1.f) * 32767));
		u16(uint16_t(v));
	}
	return bool(out);
}

} // namespace tab
