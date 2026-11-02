// SPDX-License-Identifier: Apache-2.0
// Reader for the binary Guitar Pro 3, 4 and 5 formats.
#include "readers.hpp"
#include "text.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include "limits.hpp"
#include <stdexcept>

namespace tab {
namespace {

class Reader {
public:
	Reader(const uint8_t *d, size_t n) : d_(d), n_(n) {}
	void need(size_t k) const
	{
		if (p_ + k > n_)
			throw std::runtime_error("Unexpected end of file");
	}
	void skip(size_t k) { need(k), p_ += k; }
	uint8_t u8() { need(1); return d_[p_++]; }
	int8_t i8() { return static_cast<int8_t>(u8()); }
	bool boolean() { return u8() != 0; }
	int16_t i16()
	{
		need(2);
		int16_t v = int16_t(d_[p_] | (d_[p_ + 1] << 8));
		p_ += 2;
		return v;
	}
	int32_t i32()
	{
		need(4);
		uint32_t v = uint32_t(d_[p_]) | (uint32_t(d_[p_ + 1]) << 8) | (uint32_t(d_[p_ + 2]) << 16) |
			     (uint32_t(d_[p_ + 3]) << 24);
		p_ += 4;
		return int32_t(v);
	}
	std::string str(size_t k)
	{
		need(k);
		auto s = decodeLegacyText(d_ + p_, k);
		p_ += k;
		return s;
	}
	// byte length + fixed size field
	std::string byteSizeString(size_t size)
	{
		size_t len = std::min<size_t>(u8(), size);
		auto s = str(len);
		skip(size - len);
		return s;
	}
	// int (size + 1) + byte length + chars
	std::string intByteSizeString()
	{
		int32_t total = i32();
		size_t len = u8();
		size_t field = total > 0 ? size_t(total - 1) : len;
		auto s = str(std::min(len, field));
		if (field > len)
			skip(field - len);
		return s;
	}
	std::string intSizeString()
	{
		int32_t len = i32();
		return str(size_t(std::max(0, len)));
	}

private:
	const uint8_t *d_;
	size_t n_;
	size_t p_ = 0;
};

std::vector<BendPoint> readBendPoints(Reader &r)
{
	r.u8();
	r.i32();
	int n = r.i32();
	if (n < 0 || n > 100)
		throw std::runtime_error("Corrupt bend");
	std::vector<BendPoint> pts;
	for (int i = 0; i < n; i++) {
		int pos = r.i32();
		int val = r.i32();
		r.u8();
		pts.push_back({std::clamp(pos / 60.f, 0.f, 1.f), std::clamp(val / 50.f, -limits::kMaxBendSemis, limits::kMaxBendSemis)});
	}
	return pts;
}

void readChord(Reader &r, int ver, Beat &beat)
{
	std::string name;
	if (ver >= 500) {
		r.skip(17);
		name = r.byteSizeString(21);
		r.skip(4 + 4 + 7 * 4 + 32);
	} else if (r.u8() != 0) {
		if (ver >= 400) {
			r.skip(16);
			name = r.byteSizeString(21);
			r.skip(4 + 4 + 7 * 4 + 32);
		} else {
			r.skip(25);
			name = r.byteSizeString(34);
			r.skip(4 + 6 * 4 + 36);
		}
	} else {
		int strings = ver >= 406 ? 7 : 6;
		name = r.intByteSizeString();
		int firstFret = r.i32();
		if (firstFret > 0)
			r.skip(size_t(strings) * 4);
	}
	beat.chord = trim(name);
}

Harmonic readBeatEffects(Reader &r, int ver, Beat &beat)
{
	uint8_t flags = r.u8();
	uint8_t flags2 = ver >= 400 ? r.u8() : 0;
	if (flags & 0x10)
		beat.fadeIn = true;
	if ((ver < 400 && (flags & 0x01)) || (flags & 0x02))
		beat.vibrato = true;
	if (flags & 0x20) {
		r.i8();
		if (ver < 400)
			r.skip(4);
	}
	if (flags2 & 0x04)
		readBendPoints(r); // tremolo bar (not played)
	if (flags & 0x40) {
		uint8_t up, down;
		if (ver < 500) {
			down = r.u8();
			up = r.u8();
		} else {
			up = r.u8();
			down = r.u8();
		}
		beat.brush = up ? Stroke::Up : down ? Stroke::Down : Stroke::None;
	}
	if (flags2 & 0x02) {
		int s = r.i8();
		beat.pickStroke = s == 1 ? Stroke::Up : s == 2 ? Stroke::Down : Stroke::None;
	}
	if (ver < 400) {
		if (flags & 0x04)
			return Harmonic::Natural;
		if (flags & 0x08)
			return Harmonic::Artificial;
	}
	return Harmonic::None;
}

void readMixTable(Reader &r, int ver, Beat &beat)
{
	r.i8();
	if (ver >= 500)
		r.skip(16);
	int8_t vals[6];
	for (auto &v : vals)
		v = r.i8();
	if (ver >= 500)
		r.intByteSizeString();
	int tempo = r.i32();
	for (auto v : vals)
		if (v >= 0)
			r.u8();
	if (tempo >= 0) {
		r.i8();
		if (ver >= 510)
			r.u8();
	}
	if (ver >= 400)
		r.u8();
	if (ver >= 500)
		r.i8();
	if (ver >= 510) {
		r.intByteSizeString();
		r.intByteSizeString();
	}
	if (tempo > 0)
		beat.tempo = std::clamp<double>(tempo, limits::kMinBpm, limits::kMaxBpm);
}

void readNoteEffects(Reader &r, int ver, Note &note, Beat &beat)
{
	uint8_t flags = r.u8();
	uint8_t flags2 = ver >= 400 ? r.u8() : 0;
	if (flags & 0x01)
		note.bend = readBendPoints(r);
	if (flags & 0x10) {
		Grace g;
		g.fret = r.i8();
		r.i8();
		int transition = r.i8();
		g.slide = transition == 1;
		g.hammer = transition == 3;
		r.u8();
		if (ver >= 500) {
			uint8_t gf = r.u8();
			g.dead = gf & 0x01;
			g.onBeat = gf & 0x02;
		}
		note.grace = g;
	}
	if (flags2 & 0x04) {
		int speed = r.u8();
		beat.tremolo = speed == 1 ? 8 : speed == 3 ? 32 : 16;
	}
	if (flags2 & 0x08) {
		int type = r.i8();
		if (ver >= 500) {
			note.slide = (type & 1) ? Slide::Shift : (type & 2) ? Slide::Legato : (type & 4) ? Slide::OutDown : (type & 8) ? Slide::OutUp : Slide::None;
			note.slideIn = (type & 16) ? SlideIn::Below : (type & 32) ? SlideIn::Above : SlideIn::None;
		} else {
			switch (type) {
			case 1: note.slide = Slide::Shift; break;
			case 2: note.slide = Slide::Legato; break;
			case 3: note.slide = Slide::OutDown; break;
			case 4: note.slide = Slide::OutUp; break;
			case -1: note.slideIn = SlideIn::Below; break;
			case -2: note.slideIn = SlideIn::Above; break;
			default: break;
			}
		}
	} else if (ver < 400 && (flags & 0x04)) {
		note.slide = Slide::Shift;
	}
	if (flags2 & 0x10) {
		int type = r.u8();
		if (ver >= 500) {
			switch (type) {
			case 1: note.harmonic = Harmonic::Natural; break;
			case 2: r.skip(3); note.harmonic = Harmonic::Artificial; break;
			case 3: note.harmonic = Harmonic::Tap; note.harmonicFret = r.u8(); break;
			case 4: note.harmonic = Harmonic::Pinch; break;
			case 5: note.harmonic = Harmonic::Semi; break;
			default: break;
			}
		} else {
			if (type == 1) note.harmonic = Harmonic::Natural;
			else if (type == 3) note.harmonic = Harmonic::Tap;
			else if (type == 4) note.harmonic = Harmonic::Pinch;
			else if (type == 5) note.harmonic = Harmonic::Semi;
			else if (type == 15 || type == 17 || type == 22) note.harmonic = Harmonic::Artificial;
		}
	}
	if (flags2 & 0x20) {
		r.u8();
		r.u8();
	}
	note.letRing = flags & 0x08;
	note.hammer = flags & 0x02;
	note.vibrato = flags2 & 0x40;
	note.palmMute = flags2 & 0x02;
	note.staccato = flags2 & 0x01;
}

Note readNote(Reader &r, int ver, int string, Beat &beat)
{
	Note note;
	note.string = string;
	uint8_t flags = r.u8();
	note.ghost = flags & 0x04;
	if (flags & 0x40)
		note.accent = 1;
	if (flags & 0x02)
		note.accent = 2;
	if (flags & 0x20) {
		int type = r.u8();
		note.tie = type == 2;
		note.dead = type == 3;
	}
	if ((flags & 0x01) && ver < 500)
		r.skip(2);
	if (flags & 0x10)
		note.dynamic = r.i8();
	if (flags & 0x20)
		note.fret = r.i8();
	if (flags & 0x80) {
		r.i8();
		r.i8();
	}
	if (ver >= 500) {
		if (flags & 0x01)
			r.skip(8);
		r.u8();
	}
	if (flags & 0x08)
		readNoteEffects(r, ver, note, beat);
	if (note.fret < 0 || note.fret > 99)
		note.fret = 0;
	return note;
}

Beat readBeat(Reader &r, int ver, const Track &track)
{
	static const int kTupletDen[14] = {0, 0, 0, 2, 0, 4, 4, 4, 0, 8, 8, 8, 8, 8};
	Beat beat;
	uint8_t flags = r.u8();
	if (flags & 0x01)
		beat.dots = 1;
	if (flags & 0x40) {
		r.u8();
		beat.rest = true;
	}
	int d = r.i8();
	beat.value = (d >= -2 && d <= 4) ? 1 << (d + 2) : 4;
	if (flags & 0x20) {
		int n = r.i32();
		if (n > 0 && n < 14 && kTupletDen[n]) {
			beat.tupletNum = n;
			beat.tupletDen = kTupletDen[n];
		}
	}
	if (flags & 0x02)
		readChord(r, ver, beat);
	if (flags & 0x04)
		beat.text = r.intByteSizeString();
	Harmonic all = Harmonic::None;
	if (flags & 0x08)
		all = readBeatEffects(r, ver, beat);
	if (flags & 0x10)
		readMixTable(r, ver, beat);
	uint8_t stringFlags = r.u8();
	for (int i = 6; i >= 0; i--) {
		if ((stringFlags & (1 << i)) && 6 - i < int(track.tuning.size())) {
			Note n = readNote(r, ver, 6 - i, beat);
			if (all != Harmonic::None && n.harmonic == Harmonic::None)
				n.harmonic = all;
			beat.notes.push_back(std::move(n));
		}
	}
	beat.rest = beat.notes.empty();
	if (ver >= 500) {
		int16_t flags2 = r.i16();
		// beaming relative to the previous beat (as Guitar Pro shows it)
		if (flags2 & 0x01)
			beat.beam = 1;
		else if (flags2 & 0x04)
			beat.beam = 2;
		if ((flags2 & 0x0800) && r.u8() && !beat.beam)
			beat.beam = 3;
	}
	return beat;
}

} // namespace

bool isGp3to5(const uint8_t *d, size_t n)
{
	return n > 31 && std::memcmp(d + 1, "FICHIER GUITAR PRO", 18) == 0;
}

Score readGp3to5(const uint8_t *d, size_t n)
{
	Reader r(d, n);
	std::string version = r.byteSizeString(30);
	// "FICHIER GUITAR PRO v5.10" -> 510
	auto vpos = version.find(" v");
	if (vpos == std::string::npos || vpos + 6 > version.size() || !std::isdigit((unsigned char)version[vpos + 2]) || version[vpos + 3] != '.' ||
	    !std::isdigit((unsigned char)version[vpos + 4]) || !std::isdigit((unsigned char)version[vpos + 5]))
		throw std::runtime_error("Not a Guitar Pro 3-5 file");
	int ver = (version[vpos + 2] - '0') * 100 + (version[vpos + 4] - '0') * 10 + (version[vpos + 5] - '0');
	if (ver < 300 || ver > 599)
		throw std::runtime_error("Unsupported Guitar Pro version");
	Score score;
	score.format = ver >= 500 ? "gp5" : ver >= 400 ? "gp4" : "gp3";

	score.title = r.intByteSizeString();
	score.subTitle = r.intByteSizeString();
	score.artist = r.intByteSizeString();
	score.album = r.intByteSizeString();
	score.words = r.intByteSizeString();
	score.music = ver >= 500 ? r.intByteSizeString() : score.words;
	score.copyright = r.intByteSizeString();
	score.tab = r.intByteSizeString();
	score.instructions = r.intByteSizeString();
	int notices = r.i32();
	if (notices < 0 || notices > 1000)
		throw std::runtime_error("Corrupt header");
	for (int i = 0; i < notices; i++) {
		if (i)
			score.notices += '\n';
		score.notices += r.intByteSizeString();
	}
	Feel songFeel = Feel::None; // GP3/4: one swing setting for the whole song
	if (ver < 500)
		songFeel = r.boolean() ? Feel::Triplet8th : Feel::None;
	if (ver >= 400) {
		r.i32();
		for (int i = 0; i < 5; i++) {
			r.i32();
			r.intSizeString();
		}
	}
	if (ver >= 510)
		r.skip(19);
	if (ver >= 500) {
		r.skip(30);
		for (int i = 0; i < 10; i++)
			r.intByteSizeString();
		score.tempoLabel = r.intByteSizeString();
	}
	score.tempo = std::clamp<double>(r.i32(), limits::kMinBpm, limits::kMaxBpm);
	if (ver >= 510)
		r.boolean();
	r.i32();
	if (ver >= 400)
		r.u8();
	struct Channel { int program; };
	std::vector<Channel> channels(64);
	for (auto &c : channels) {
		c.program = r.i32();
		r.skip(8);
	}
	if (ver >= 500)
		r.skip(38 + 4);

	int barCount = r.i32();
	int trackCount = r.i32();
	if (barCount < 0 || barCount > limits::kMaxBars || trackCount < 0 || trackCount > limits::kMaxTracks)
		throw std::runtime_error("Corrupt header");

	MasterBar prev;
	for (int i = 0; i < barCount; i++) {
		uint8_t flags = r.u8();
		MasterBar mb;
		mb.num = prev.num;
		mb.den = prev.den;
		if (flags & 0x01)
			mb.num = r.u8();
		if (flags & 0x02)
			mb.den = r.u8();
		mb.repeatStart = flags & 0x04;
		if (flags & 0x08)
			mb.repeatPlays = r.u8() + (ver >= 500 ? 0 : 1);
		if ((flags & 0x10) && ver < 500) {
			int k = r.u8();
			mb.alternate = (k > 0 && k <= 8) ? 1u << (k - 1) : uint32_t(k);
		}
		if (flags & 0x20) {
			mb.marker = r.intByteSizeString();
			r.skip(4);
		}
		if (flags & 0x40) {
			r.i8();
			r.u8();
		}
		if (ver >= 500) {
			if (flags & 0x03)
				r.skip(4);
			uint8_t alt = r.u8();
			if (flags & 0x10)
				mb.alternate = alt;
			int feel = r.u8();
			mb.feel = feel == 1 ? Feel::Triplet8th : feel == 2 ? Feel::Triplet16th : Feel::None;
			r.u8();
		} else {
			mb.feel = songFeel;
		}
		mb.doubleBar = flags & 0x80;
		if (mb.num <= 0 || mb.den <= 0 || mb.num > 64 || mb.den > 64) {
			mb.num = 4;
			mb.den = 4;
		}
		score.masterBars.push_back(mb);
		prev = mb;
	}

	for (int t = 0; t < trackCount; t++) {
		Track track;
		uint8_t flags = r.u8();
		track.name = r.byteSizeString(40);
		track.percussion = flags & 0x01;
		int strings = r.i32();
		if (strings < 0 || strings > 7)
			throw std::runtime_error("Corrupt track");
		for (int i = 0; i < 7; i++) {
			int tune = r.i32();
			if (i < strings)
				track.tuning.push_back(std::clamp(tune, 0, 127));
		}
		r.i32();
		int channel = r.i32() - 1;
		r.i32();
		r.i32();
		track.capo = r.i32();
		r.skip(4);
		if (ver >= 500)
			r.skip(45);
		if (ver >= 510) {
			r.skip(4);
			r.intByteSizeString();
			r.intByteSizeString();
		}
		if (channel == 9)
			track.percussion = true;
		score.tracks.push_back(std::move(track));
	}

	for (int i = 0; i < barCount; i++) {
		for (auto &track : score.tracks) {
			Bar bar;
			int voices = ver >= 500 ? 2 : 1;
			if (ver >= 500)
				r.u8();
			for (int v = 0; v < voices; v++) {
				Voice beats;
				int count = r.i32();
				if (count < 0 || count > limits::kMaxBeatsPerVoice)
					throw std::runtime_error("Corrupt bar data");
				for (int k = 0; k < count; k++)
					beats.push_back(readBeat(r, ver, track));
				bar.voices.push_back(std::move(beats));
			}
			track.bars.push_back(std::move(bar));
		}
	}
	finishScore(score);
	return score;
}

} // namespace tab
