// SPDX-License-Identifier: Apache-2.0
#include "layout.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace tab {
namespace {

const char *kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

struct Slot {
	int tick;
	float w, min;
};

struct BarInfo {
	int index;
	const MasterBar *mb;
	const Bar *bar;
	std::vector<Slot> slots;
	bool showTime;
	float pre, post, natural;
	int sys = -1;
	std::map<int, float> slotX;
};

struct System {
	std::vector<BarInfo *> bars;
	float stretch = 1;
	bool last = false;
};

struct Span {
	int bar, k, len;
	float x0, x1;
};

std::string bendLabel(float semis)
{
	double steps = std::round(semis * 2) / 4.0;
	if (steps <= 0)
		return "";
	if (steps == 0.25) return "\xC2\xBC";
	if (steps == 0.5) return "\xC2\xBD";
	if (steps == 1) return "full";
	int whole = int(std::floor(steps));
	double frac = steps - whole;
	std::string s = std::to_string(whole);
	if (frac == 0.5) s += "\xC2\xBD";
	else if (frac == 0.25) s += "\xC2\xBC";
	else if (frac == 0.75) s += "\xC2\xBE";
	return s;
}

// Bend points reduced to the turns Guitar Pro draws: rises, holds and releases.
std::vector<BendPoint> bendShape(const std::vector<BendPoint> &in)
{
	std::vector<BendPoint> pts;
	for (auto p : in) {
		p.t = std::clamp(p.t, 0.f, 1.f);
		p.semis = std::round(p.semis * 4) / 4;
		if (!pts.empty() && p.t < pts.back().t)
			continue;
		if (pts.size() >= 2) {
			float d1 = pts.back().semis - pts[pts.size() - 2].semis, d2 = p.semis - pts.back().semis;
			if ((d1 > 0 && d2 > 0) || (d1 < 0 && d2 < 0) || (d1 == 0 && d2 == 0)) {
				pts.back() = p; // same direction: extend the segment
				continue;
			}
		}
		if (!pts.empty() && p.semis == pts.back().semis && p.t == pts.back().t)
			continue;
		pts.push_back(p);
	}
	return pts;
}

class Builder {
public:
	explicit Builder(Layout &l) : l_(l) {}
	void line(Role r, float x0, float y0, float x1, float y1, float w = 1)
	{
		Prim p;
		p.type = Prim::Line;
		p.role = r;
		p.x0 = x0, p.y0 = y0, p.x1 = x1, p.y1 = y1, p.stroke = w;
		l_.prims.push_back(std::move(p));
	}
	void rect(Role r, float x, float y, float w, float h, int beatKey = -1)
	{
		Prim p;
		p.type = Prim::Rect;
		p.role = r;
		p.x0 = x, p.y0 = y, p.x1 = x + w, p.y1 = y + h, p.beatKey = beatKey;
		l_.prims.push_back(std::move(p));
	}
	void circle(Role r, float cx, float cy, float radius)
	{
		Prim p;
		p.type = Prim::Circle;
		p.role = r;
		p.x0 = cx, p.y0 = cy, p.x1 = radius;
		l_.prims.push_back(std::move(p));
	}
	Prim &text(Role r, float x, float y, std::string s, float size, uint8_t align = 1)
	{
		Prim p;
		p.type = Prim::Text;
		p.role = r;
		p.x0 = x, p.y0 = y, p.size = size, p.align = align, p.text = std::move(s);
		l_.prims.push_back(std::move(p));
		return l_.prims.back();
	}
	Prim &path(Role r, std::vector<PathOp> ops, float w = 1.2f)
	{
		Prim p;
		p.type = Prim::Path;
		p.role = r;
		p.path = std::move(ops);
		p.stroke = w;
		l_.prims.push_back(std::move(p));
		return l_.prims.back();
	}

private:
	Layout &l_;
};

PathOp M(float x, float y) { return {'M', {x, 0, 0}, {y, 0, 0}}; }
PathOp L(float x, float y) { return {'L', {x, 0, 0}, {y, 0, 0}}; }
PathOp Q(float cx, float cy, float x, float y) { return {'Q', {cx, x, 0}, {cy, y, 0}}; }
PathOp C(float x1, float y1, float x2, float y2, float x, float y) { return {'C', {x1, x2, x}, {y1, y2, y}}; }
PathOp Z() { return {'Z', {0, 0, 0}, {0, 0, 0}}; }

} // namespace

// Width of bold text for placing the next item after it: wide enough for the widest of the fonts
// used (Segoe UI on Windows, Helvetica on macOS, DejaVu Sans on Linux).
float textWidth(const std::string &s, float size)
{
	size_t chars = 0;
	for (unsigned char c : s)
		chars += (c & 0xC0) != 0x80; // UTF-8: count characters, not bytes
	return float(chars) * size * 0.68f;
}

std::string fretText(float f)
{
	int tenths = int(std::lround(f * 10));
	return tenths % 10 ? std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) : std::to_string(tenths / 10);
}

std::string noteLabel(const Note &n)
{
	if (n.dead)
		return "x";
	if (n.harmonic == Harmonic::Natural)
		return "<" + fretText(n.harmonicFret > 0 ? n.harmonicFret : float(n.fret)) + ">";
	if (n.harmonic == Harmonic::Artificial || n.harmonic == Harmonic::Pinch)
		return std::to_string(n.fret);
	if (n.ghost || n.tie)
		return "(" + std::to_string(n.fret) + ")";
	return std::to_string(n.fret);
}

// Small text written after the fret, as Guitar Pro does: the trill fret, or where a harmonic sounds.
std::string noteSuffix(const Note &n)
{
	if (n.trill >= 0)
		return "(" + std::to_string(n.trill) + ")";
	if (n.harmonicFret > 0 && n.harmonic != Harmonic::None && n.harmonic != Harmonic::Natural)
		return "<" + fretText(n.fret + n.harmonicFret) + ">";
	return "";
}

// Whammy amounts in tones like Guitar Pro: -1, -½, +1½ ...
std::string whammyLabel(float semis)
{
	std::string m = bendLabel(std::abs(semis));
	if (m == "full")
		m = "1";
	return m.empty() ? "0" : (semis < 0 ? "-" : "+") + m;
}

const char *dynamicName(int d)
{
	static const char *kNames[9] = {"", "ppp", "pp", "p", "mp", "mf", "f", "ff", "fff"};
	return d >= 1 && d <= 8 ? kNames[d] : "";
}

const char *jumpName(Jump j)
{
	switch (j) {
	case Jump::DaCapo: return "D.C.";
	case Jump::DaCapoAlCoda: return "D.C. al Coda";
	case Jump::DaCapoAlDoubleCoda: return "D.C. al Double Coda";
	case Jump::DaCapoAlFine: return "D.C. al Fine";
	case Jump::DaSegno: return "D.S.";
	case Jump::DaSegnoAlCoda: return "D.S. al Coda";
	case Jump::DaSegnoAlDoubleCoda: return "D.S. al Double Coda";
	case Jump::DaSegnoAlFine: return "D.S. al Fine";
	case Jump::DaSegnoSegno: return "D.S.S.";
	case Jump::DaSegnoSegnoAlCoda: return "D.S.S. al Coda";
	case Jump::DaSegnoSegnoAlDoubleCoda: return "D.S.S. al Double Coda";
	case Jump::DaSegnoSegnoAlFine: return "D.S.S. al Fine";
	case Jump::DaCoda: return "To Coda";
	case Jump::DaDoubleCoda: return "To Double Coda";
	default: return "";
	}
}

const char *harmonicName(Harmonic h)
{
	switch (h) {
	case Harmonic::Natural: return "N.H.";
	case Harmonic::Artificial: return "A.H.";
	case Harmonic::Tap: return "T.H.";
	case Harmonic::Pinch: return "P.H.";
	case Harmonic::Semi: return "S.H.";
	default: return "";
	}
}

Layout layoutTrack(const Score &score, int trackIndex, const LayoutOptions &o)
{
	Layout out;
	out.scale = o.scale;
	if (trackIndex < 0 || trackIndex >= int(score.tracks.size()))
		return out;
	const Track &track = score.tracks[size_t(trackIndex)];
	const float S = o.scale;
	const int strings = std::max(1, int(track.tuning.empty() ? 6 : track.tuning.size()));
	const float gap = 13 * S, font = 12.5f * S, digitW = font * 0.6f, margin = 8 * S;
	const float clefW = o.labels == 2 ? 6 * S : 26 * S;
	static const Bar kEmpty;

	// horizontal metrics per bar
	std::vector<BarInfo> bars;
	bars.reserve(score.masterBars.size());
	for (size_t i = 0; i < score.masterBars.size(); i++) {
		BarInfo b;
		b.index = int(i);
		b.mb = &score.masterBars[i];
		b.bar = i < track.bars.size() ? &track.bars[i] : &kEmpty;
		std::map<int, Slot> slots;
		for (auto &v : b.bar->voices)
			for (auto &beat : v) {
				size_t digits = 1;
				float side = 0; // room for a grace note or brush arrow before, or a suffix after
				for (auto &n : beat.notes) {
					digits = std::max(digits, noteLabel(n).size());
					if (n.grace)
						side = std::max(side, (n.grace->dead ? 1 : std::to_string(n.grace->fret).size()) * digitW * 0.75f + 6 * S);
					if (auto suffix = noteSuffix(n); !suffix.empty())
						side = std::max(side, suffix.size() * digitW * 0.7f + 3 * S);
				}
				if (beat.brush != Stroke::None && !beat.rest)
					side = std::max(side, 8 * S);
				float minW = digits * digitW + 7 * S + 2 * side;
				float w = std::max(minW, (16 + 26 * std::sqrt(std::min(beat.ticks, kQuarter * 4) / float(kQuarter))) * S * o.spacing);
				auto it = slots.find(beat.start);
				if (it == slots.end())
					slots[beat.start] = {beat.start, w, minW};
				else if (w < it->second.w)
					it->second.w = std::max(it->second.min, w);
			}
		for (auto &kv : slots)
			b.slots.push_back(kv.second);
		if (b.slots.empty())
			b.slots.push_back({0, 40 * S, 10 * S});
		const MasterBar *prev = i ? &score.masterBars[i - 1] : nullptr;
		b.showTime = !prev || prev->num != b.mb->num || prev->den != b.mb->den;
		b.pre = 12 * S + (b.showTime ? 24 * S : 0) + (b.mb->repeatStart ? 14 * S : 0);
		b.post = 6 * S + (b.mb->repeatPlays ? 10 * S : 0);
		float content = 0;
		for (auto &s : b.slots)
			content += s.w;
		b.natural = b.pre + content + b.post;
		bars.push_back(std::move(b));
	}

	// rows
	std::vector<System> systems;
	if (o.horizontal) {
		System s;
		for (auto &b : bars)
			s.bars.push_back(&b);
		systems.push_back(std::move(s));
	} else {
		float avail = o.width - margin * 2 - clefW;
		System row;
		float w = 0;
		for (auto &b : bars) {
			if (!row.bars.empty() && w + b.natural > avail) {
				systems.push_back(std::move(row));
				row = System();
				w = 0;
			}
			row.bars.push_back(&b);
			w += b.natural;
		}
		if (!row.bars.empty()) {
			row.last = true;
			systems.push_back(std::move(row));
		}
		for (auto &sys : systems) {
			float nat = 0, fixed = 0, floor = 0;
			for (auto *b : sys.bars) {
				nat += b->natural;
				fixed += b->pre + b->post;
				for (auto &sl : b->slots)
					floor = std::max(floor, sl.min / sl.w);
			}
			float fill = (sys.last && nat < avail * 0.7f) ? 1 : (avail - fixed) / std::max(1.f, nat - fixed);
			sys.stretch = std::max(floor, fill);
		}
	}

	Builder B(out);
	out.bars.resize(bars.size());
	float y = margin, maxWidth = 0;
	int dynBefore = (score.format == "gp7" || score.format == "gpx") ? 5 : 6; // last dynamic written
	const int lastBar = int(bars.size()) - 1;
	for (size_t si = 0; si < systems.size(); si++) {
		auto &sys = systems[si];
		auto any = [&](auto pred) {
			for (auto *b : sys.bars)
				for (auto &v : b->bar->voices)
					for (auto &beat : v)
						if (pred(beat))
							return true;
			return false;
		};
		bool hasChords = o.chords && any([](const Beat &b) { return !b.chord.empty(); });
		bool hasText = any([](const Beat &b) { return !b.text.empty(); });
		for (auto *b : sys.bars)
			hasText = hasText || !b->mb->marker.empty() || b->mb->alternate || b->mb->repeatPlays > 2;
		auto noteAny = [&](auto pred) { return any([&](const Beat &b) { return std::any_of(b.notes.begin(), b.notes.end(), pred); }); };
		bool hasBend = o.techniques && noteAny([](const Note &n) { return !n.bend.empty(); });
		const bool hasWhammy = o.techniques && any([](const Beat &b) { return !b.whammy.empty() && !b.rest; });
		// Effect lanes above the tab: each kind of effect keeps one lane, as Guitar Pro stacks them, but
		// kinds that never meet along the row share a lane, so one long line of tab stays low. The kinds
		// nearest the strings come first. Bar and beat positions are worked out here ahead of drawing.
		enum Kind { KAccent, KPick, KTSP, KFade, KTrem, KVib, KTrill, KHarm, KPM, KRing, KCount };
		std::vector<std::pair<float, float>> reach[KCount]; // x ranges each kind covers
		if (o.techniques) {
			float px = margin + clefW;
			for (auto *b : sys.bars) {
				float barX = px, cx = barX;
				if (b->mb->repeatStart)
					cx += 14 * S;
				if (b->showTime)
					cx += 24 * S;
				cx += 12 * S;
				for (auto &sl : b->slots) {
					b->slotX[sl.tick] = cx + sl.min / 2;
					cx += sl.w * sys.stretch;
				}
				cx += b->post - (b->mb->repeatPlays ? 10 * S : 0);
				cx += b->mb->repeatPlays ? 10 * S : b->index == lastBar ? 1 * S : 0;
				px = cx;
				if (b->bar->voices.empty())
					continue;
				const Voice &voice = b->bar->voices[0];
				for (size_t k = 0; k < voice.size(); k++) {
					const Beat &beat = voice[k];
					if (beat.rest)
						continue;
					auto at = b->slotX.find(beat.start);
					float bx = at != b->slotX.end() ? at->second : barX + 10 * S;
					float endX = cx - 4 * S;
					if (k + 1 < voice.size() && b->slotX.count(voice[k + 1].start))
						endX = b->slotX[voice[k + 1].start];
					const float effEnd = std::max(bx + 12 * S, endX - 4 * S);
					auto has = [&](auto pred) { return std::any_of(beat.notes.begin(), beat.notes.end(), pred); };
					auto add = [&](Kind kd, float a, float c) { reach[kd].push_back({a, c}); };
					if (has([](auto &n) { return n.accent > 0 || n.staccato; }))
						add(KAccent, bx - 6 * S, bx + 10 * S);
					if (beat.pickStroke != Stroke::None)
						add(KPick, bx - 6 * S, bx + 6 * S);
					if (beat.technique)
						add(KTSP, bx - 6 * S, bx + 6 * S);
					if (beat.fadeIn)
						add(KFade, bx - 3 * S, effEnd + 1 * S);
					if (beat.tremolo > 0)
						add(KTrem, bx - 6 * S, bx + 6 * S);
					if (beat.vibrato || has([](auto &n) { return n.vibrato || n.wideVibrato; }))
						add(KVib, bx - 3 * S, effEnd + 1 * S);
					if (has([](auto &n) { return n.trill >= 0; }))
						add(KTrill, bx - 6 * S, effEnd + 1 * S);
					// spans reach their label at the start and the hook at the end
					if (has([](auto &n) { return n.harmonic != Harmonic::None; }))
						add(KHarm, bx - 6 * S, std::max(effEnd, bx + 18 * S) + 6 * S);
					if (has([](auto &n) { return n.palmMute; }))
						add(KPM, bx - 6 * S, std::max(effEnd, bx + 18 * S) + 6 * S);
					if (has([](auto &n) { return n.letRing; }))
						add(KRing, bx - 6 * S, std::max(effEnd, bx + 40 * S) + 6 * S);
				}
			}
		}
		// pack: merge each kind's ranges, then put it in the first lane it does not meet
		auto merged = [](std::vector<std::pair<float, float>> v) {
			std::sort(v.begin(), v.end());
			std::vector<std::pair<float, float>> m;
			for (auto &r : v)
				if (!m.empty() && r.first <= m.back().second)
					m.back().second = std::max(m.back().second, r.second);
				else
					m.push_back(r);
			return m;
		};
		std::vector<std::vector<std::pair<float, float>>> lanes;
		int laneOf[KCount];
		for (int kd = 0; kd < KCount; kd++) {
			laneOf[kd] = -1;
			if (reach[kd].empty())
				continue;
			auto mine = merged(reach[kd]);
			auto meets = [&](const std::vector<std::pair<float, float>> &lane) {
				size_t i = 0, j = 0;
				while (i < lane.size() && j < mine.size()) {
					if (lane[i].first < mine[j].second - 0.5f && mine[j].first < lane[i].second - 0.5f)
						return true;
					if (lane[i].second < mine[j].second)
						i++;
					else
						j++;
				}
				return false;
			};
			size_t l = 0;
			while (l < lanes.size() && meets(lanes[l]))
				l++;
			if (l == lanes.size())
				lanes.emplace_back();
			lanes[l].insert(lanes[l].end(), mine.begin(), mine.end());
			lanes[l] = merged(lanes[l]);
			laneOf[kd] = int(l);
		}
		bool hasDir = false, hasFermata = false;
		for (auto *b : sys.bars) {
			hasDir = hasDir || b->mb->targets || b->mb->jump != Jump::None;
			hasFermata = hasFermata || !b->mb->fermatas.empty();
		}
		// dynamics are written where they change (the default, mf in Guitar Pro 6-8 and f before, is not)
		const int defaultDyn = (score.format == "gp7" || score.format == "gpx") ? 5 : 6;
		bool hasDyn = false;
		{
			int last = dynBefore;
			for (auto *b : sys.bars)
				if (!b->bar->voices.empty())
					for (auto &beat : b->bar->voices[0]) {
						if (beat.notes.empty())
							continue;
						int d = 0; // the loudest note of the beat, as drawn below
						for (auto &n : beat.notes)
							d = std::max(d, n.dynamic >= 1 ? n.dynamic : defaultDyn);
						hasDyn = hasDyn || d != last;
						last = d;
					}
		}
		// tempo and swing marks (Guitar Pro writes them above the first bar and where they change)
		auto barTempo = [](const BarInfo *b) {
			double t = 0;
			for (auto &tc : b->mb->tempoChanges)
				if (tc.ratio <= 0.001)
					t = tc.bpm;
			for (auto &v : b->bar->voices)
				for (auto &beat : v)
					if (beat.tempo > 0 && beat.start == 0)
						t = beat.tempo;
			return t;
		};
		// tempo changes inside a bar: (tick, bpm)
		auto midTempos = [](const BarInfo *b) {
			std::vector<std::pair<int, double>> out;
			const int len = barTicks(*b->mb);
			for (auto &tc : b->mb->tempoChanges)
				if (tc.ratio > 0.001)
					out.push_back({int(tc.ratio * len), tc.bpm});
			if (!b->bar->voices.empty())
				for (auto &beat : b->bar->voices[0])
					if (beat.tempo > 0 && beat.start > 0)
						out.push_back({beat.start, beat.tempo});
			return out;
		};
		bool hasTempo = false;
		for (auto *b : sys.bars) {
			const MasterBar *prev = b->index ? &score.masterBars[size_t(b->index - 1)] : nullptr;
			if (b->index == 0 || barTempo(b) > 0 || (prev ? prev->feel : Feel::None) != b->mb->feel || !midTempos(b).empty())
				hasTempo = true;
		}
		float top = 4 * S;
		float rowDir = top; if (hasDir) top += 18 * S;
		float rowTempo = top; if (hasTempo) top += 17 * S;
		float rowChord = top; if (hasChords) top += 16 * S;
		float rowText = top; if (hasText) top += 16 * S;
		const float lane = 13 * S, lanesTop = top;
		top += float(lanes.size()) * lane;
		auto rowOf = [&](Kind kd) { return laneOf[kd] < 0 ? lanesTop : lanesTop + float(int(lanes.size()) - 1 - laneOf[kd]) * lane; };
		const float rowRing = rowOf(KRing), rowPM = rowOf(KPM), rowHarm = rowOf(KHarm), rowTrill = rowOf(KTrill), rowVib = rowOf(KVib);
		const float rowTrem = rowOf(KTrem), rowFade = rowOf(KFade), rowTSP = rowOf(KTSP), rowPick = rowOf(KPick), rowAccent = rowOf(KAccent);
		float rowWhammy = top; if (hasWhammy) top += 24 * S;
		float rowFermata = top; if (hasFermata) top += 14 * S;
		if (hasBend) top += 18 * S;
		if (o.barNumbers && !hasText && !hasChords) top += 6 * S;
		const float staffTop = y + top + 6 * S, staffH = gap * (strings - 1), staffBottom = staffTop + staffH;
		const float rhythmTop = staffBottom + 7 * S;
		const float dynY = staffBottom + (o.rhythm ? 44 * S : 16 * S);
		const float bottom = staffBottom + (o.rhythm ? 34 * S : 10 * S) + (hasDyn && o.techniques ? 16 * S : 0);
		auto sy = [&](int s) { return staffTop + s * gap; };
		std::vector<std::vector<std::pair<float, float>>> gaps{size_t(strings)};
		float x = margin;
		const size_t staffStart = out.prims.size(); // staff lines are added last but drawn first

		if (o.labels == 0 && strings >= 3) {
			float mid = (staffTop + staffBottom) / 2, step = std::min(gap * 1.15f, staffH / 3);
			const char *tab[3] = {"T", "A", "B"};
			for (int k = 0; k < 3; k++)
				B.text(Role::Clef, x + 9 * S, mid + (k - 1) * step, tab[k], font * 0.95f).bold = true;
			for (int s = 0; s < strings; s++)
				gaps[size_t(s)].push_back({x + 2 * S, x + 16 * S});
		} else if (o.labels == 1) {
			for (int s = 0; s < strings; s++) {
				int t = s < int(track.tuning.size()) ? track.tuning[size_t(s)] : 40;
				B.text(Role::StringName, x + 8 * S, sy(s), kNoteNames[((t % 12) + 12) % 12], font * 0.8f).bold = true;
			}
		}
		const float lineStart = x + (o.labels == 1 ? 18 * S : 0);
		x += clefW;
		B.line(Role::BarLine, lineStart, staffTop, lineStart, staffBottom, 1.2f);
		SysGeo sg{y, staffTop, staffBottom, 0, lineStart, 0};

		auto repeatDots = [&](float cx) {
			float mid = (staffTop + staffBottom) / 2, off = std::max(gap * 0.5f, staffH * 0.15f);
			B.circle(Role::RepeatDot, cx, mid - off, 1.9f * S);
			B.circle(Role::RepeatDot, cx, mid + off, 1.9f * S);
		};

		for (auto *b : sys.bars) {
			const MasterBar &mb = *b->mb;
			float barX = x, cx = barX;
			if (mb.repeatStart) {
				B.rect(Role::BarThick, cx + 1 * S, staffTop, 3 * S, staffH);
				B.line(Role::BarLine, cx + 7 * S, staffTop, cx + 7 * S, staffBottom, 1.2f);
				repeatDots(cx + 11 * S);
				cx += 14 * S;
			}
			if (b->showTime) {
				float tx = cx + 13 * S, mid = (staffTop + staffBottom) / 2;
				float fs = std::min(staffH * 0.42f, 24 * S);
				B.text(Role::TimeSig, tx, mid - staffH * 0.22f, std::to_string(mb.num), fs).bold = true;
				B.text(Role::TimeSig, tx, mid + staffH * 0.22f, std::to_string(mb.den), fs).bold = true;
				for (int s = 0; s < strings; s++)
					gaps[size_t(s)].push_back({tx - 8 * S, tx + 8 * S});
				cx += 24 * S;
			}
			cx += 12 * S;
			for (auto &sl : b->slots) {
				b->slotX[sl.tick] = cx + sl.min / 2;
				cx += sl.w * sys.stretch;
			}
			cx += b->post - (mb.repeatPlays ? 10 * S : 0);
			if (mb.repeatPlays) {
				repeatDots(cx - 1 * S);
				cx += 4 * S;
				B.line(Role::BarLine, cx, staffTop, cx, staffBottom, 1.2f);
				B.rect(Role::BarThick, cx + 3 * S, staffTop, 3 * S, staffH);
				cx += 6 * S;
				if (mb.repeatPlays > 2)
					B.text(Role::SmallText, cx - 4 * S, y + rowText + 10 * S, "\xC3\x97" + std::to_string(mb.repeatPlays), font * 0.75f, 2);
			} else if (b->index == lastBar) {
				B.line(Role::BarLine, cx - 5 * S, staffTop, cx - 5 * S, staffBottom, 1.2f);
				B.rect(Role::BarThick, cx - 2 * S, staffTop, 3 * S, staffH);
				cx += 1 * S;
			} else {
				B.line(Role::BarLine, cx, staffTop, cx, staffBottom, 1.2f);
				if (mb.doubleBar)
					B.line(Role::BarLine, cx - 3 * S, staffTop, cx - 3 * S, staffBottom, 1.2f);
			}
			float barEnd = cx;
			if (o.barNumbers)
				B.text(Role::BarNumber, barX + 2 * S, staffTop - 5 * S, std::to_string(b->index + 1), font * 0.68f, 0);
			if (mb.targets) {
				// signs at the start of the bar: segno, coda, fine
				std::string sign;
				const char *kSigns[5] = {"\xF0\x9D\x84\x8C", "\xF0\x9D\x84\x8C\xF0\x9D\x84\x8C", "\xF0\x9D\x84\x8B", "\xF0\x9D\x84\x8B\xF0\x9D\x84\x8B", "Fine"};
				for (int k = 0; k < 5; k++)
					if (mb.targets & (1 << k))
						sign += (sign.empty() ? "" : " ") + std::string(kSigns[k]);
				B.text(Role::Marker, barX + 3 * S, y + rowDir + 9 * S, sign, font * (mb.targets == kFine ? 0.85f : 1.5f), 0).bold = true;
			}
			if (mb.jump != Jump::None) {
				auto &t = B.text(Role::Marker, barEnd - 4 * S, y + rowDir + 9 * S, jumpName(mb.jump), font * 0.82f, 2);
				t.bold = t.italic = true;
			}
			for (auto &f : mb.fermatas) {
				auto at = b->slotX.lower_bound(int(f.ratio * barTicks(mb)));
				float fx = at != b->slotX.end() ? at->second : barEnd - 8 * S, fy = y + rowFermata + 10 * S;
				B.path(Role::Technique, {M(fx - 6 * S, fy), C(fx - 6 * S, fy - 8 * S, fx + 6 * S, fy - 8 * S, fx + 6 * S, fy)}, 1.4f);
				B.circle(Role::Arrow, fx, fy - 1.5f * S, 1.3f * S);
			}
			if (!mb.marker.empty())
				B.text(Role::Marker, barX + 3 * S, y + rowText + 11 * S, mb.marker, font * 0.9f, 0).bold = true;
			{
				float tx = barX + 3 * S, ty = y + rowTempo + 11 * S;
				double bpmHere = b->index == 0 ? score.tempo : barTempo(b);
				if (b->index == 0) {
					double t = barTempo(b);
					if (t > 0)
						bpmHere = t;
					if (!score.tempoLabel.empty()) {
						B.text(Role::BarNumber, tx, ty, score.tempoLabel, font * 0.82f, 0).bold = true;
						tx += textWidth(score.tempoLabel, font * 0.82f) + 10 * S;
					}
				}
				if (bpmHere > 0) {
					// U+2669 = quarter note
					std::string mark = "\xE2\x99\xA9 = " + std::to_string(int(std::lround(bpmHere)));
					B.text(Role::BarNumber, tx, ty, mark, font * 0.82f, 0).bold = true;
					tx += textWidth(mark, font * 0.82f) + 10 * S;
				}
				if (b->index == 0 && track.capo > 0) {
					std::string capo = "Capo fret " + std::to_string(track.capo);
					B.text(Role::BarNumber, tx, ty, capo, font * 0.82f, 0).bold = true;
					tx += textWidth(capo, font * 0.82f) + 10 * S;
				}
				for (auto [tick, bpm] : midTempos(b)) {
					auto at = b->slotX.lower_bound(tick);
					float mx = at != b->slotX.end() ? at->second : barEnd - 12 * S;
					B.text(Role::BarNumber, mx - 4 * S, ty, "\xE2\x99\xA9 = " + std::to_string(int(std::lround(bpm))), font * 0.82f, 0).bold = true;
				}
				const MasterBar *prev = b->index ? &score.masterBars[size_t(b->index - 1)] : nullptr;
				if ((prev ? prev->feel : Feel::None) != mb.feel) {
					// swing: two 8ths (or 16ths) = the uneven pair they are played as
					const bool six = mb.feel == Feel::Triplet16th || mb.feel == Feel::Dotted16th || mb.feel == Feel::Scottish16th;
					std::string even = six ? "\xE2\x99\xAC" : "\xE2\x99\xAB"; // beamed 16ths / 8ths
					std::string played;
					switch (mb.feel) {
					case Feel::Triplet8th: case Feel::Triplet16th: played = "\xE2\x99\xA9\xE2\x99\xAA"; break; // quarter + eighth, as a triplet
					case Feel::Dotted8th: case Feel::Dotted16th: played = "\xE2\x99\xAA.\xE2\x99\xAC"; break;
					case Feel::Scottish8th: case Feel::Scottish16th: played = "\xE2\x99\xAC\xE2\x99\xAA."; break;
					default: played = even; break;
					}
					if (mb.feel == Feel::None)
						B.text(Role::BarNumber, tx, ty, "straight", font * 0.75f, 0).italic = true;
					else {
						std::string text = "(" + even + " = " + played + ")";
						auto &t = B.text(Role::BarNumber, tx, ty, text, font * 0.9f, 0);
						t.bold = true;
						if (mb.feel == Feel::Triplet8th || mb.feel == Feel::Triplet16th)
							B.text(Role::SmallText, tx + font * 3.05f, ty - font * 0.75f, "3", font * 0.55f).bold = true;
					}
				}
			}
			if (mb.alternate) {
				// one bracket over all the bars of an ending in this row, numbered once
				const bool fromPrev = b != sys.bars.front() && score.masterBars[size_t(b->index - 1)].alternate == mb.alternate;
				const bool toNext = b != sys.bars.back() && score.masterBars[size_t(b->index + 1)].alternate == mb.alternate;
				float ay = y + rowText + 2 * S, x1 = toNext ? barEnd : barEnd - 4 * S;
				if (fromPrev)
					B.path(Role::Technique, {M(barX, ay), L(x1, ay)});
				else {
					std::string nums;
					for (int k = 0; k < 8; k++)
						if (mb.alternate & (1u << k))
							nums += (nums.empty() ? "" : ", ") + std::to_string(k + 1) + ".";
					B.path(Role::Technique, {M(barX + 2 * S, ay + 12 * S), L(barX + 2 * S, ay), L(x1, ay)});
					B.text(Role::SmallText, barX + 6 * S, ay + 10 * S, nums, font * 0.75f, 0);
				}
			}
			out.bars[size_t(b->index)] = {b->index, int(si), barX, barEnd};
			b->sys = int(si);
			x = barEnd;
		}

		struct RhythmItem {
			const Beat *beat;
			float x;
			int bar;
			bool rest;
		};
		std::vector<RhythmItem> rhythm;
		std::vector<Span> pmSpans, ringSpans, harmSpans;
		std::vector<Harmonic> harmTypes;
		// bend in progress per string (voice 1): where its last segment ended and at what pitch
		struct BendRun {
			bool active = false;
			float x = 0, level = 0;
		};
		std::vector<BendRun> bendRun(static_cast<size_t>(strings));
		const float bendTop = staffTop - 8 * S;
		auto addSpan = [](std::vector<Span> &spans, int bar, int k, float bx, int len) {
			if (!spans.empty()) {
				auto &l = spans.back();
				if (l.bar == bar && l.k == k - 1) { l.k = k; l.x1 = bx; return; }
				if (l.bar == bar - 1 && k == 0 && l.k == l.len - 1) { l.bar = bar; l.k = k; l.len = len; l.x1 = bx; return; }
			}
			spans.push_back({bar, k, len, bx, bx});
		};

		for (auto *b : sys.bars) {
			float barX = out.bars[size_t(b->index)].x0;
			for (size_t vi = 0; vi < b->bar->voices.size(); vi++) {
				const Voice &voice = b->bar->voices[vi];
				for (size_t k = 0; k < voice.size(); k++) {
					const Beat &beat = voice[k];
					auto sxIt = b->slotX.find(beat.start);
					float bx = sxIt != b->slotX.end() ? sxIt->second : barX + 10 * S;
					if (beat.rest && beat.fullBar && voice.size() == 1) // a whole-bar rest sits in the middle of the bar
						bx = (barX + out.bars[size_t(b->index)].x1) / 2;
					int key = b->index * 4096 + int(k);
					if (vi == 0)
						out.beats[key] = bx;
					if (!beat.chord.empty() && o.chords) {
						auto &t = B.text(Role::Chord, bx, y + rowChord + 11 * S, beat.chord, font * 0.9f);
						t.bold = t.italic = true;
					}
					if (!beat.text.empty())
						B.text(Role::BeatText, bx, y + rowText + 11 * S, beat.text, font * 0.8f).italic = true;
					float mid = (staffTop + staffBottom) / 2;
					if (beat.rest) {
						if (vi != 0)
							continue;
						rhythm.push_back({&beat, bx, b->index, true});
						if (beat.fullBar || beat.value == 1)
							B.rect(Role::Rest, bx - 5 * S, mid - 4 * S, 10 * S, 3.5f * S, key);
						else if (beat.value == 2)
							B.rect(Role::Rest, bx - 5 * S, mid - 0.5f * S, 10 * S, 3.5f * S, key);
						else if (beat.value == 4)
							B.path(Role::Rest, {M(bx - 2 * S, mid - 9 * S), L(bx + 2 * S, mid - 4 * S), L(bx - 2 * S, mid), L(bx + 2 * S, mid + 5 * S),
									    C(bx - 2 * S, mid + 3 * S, bx - 3 * S, mid + 7 * S, bx + 1 * S, mid + 10 * S)}, 1.8f);
						else {
							int flags = std::max(1, int(std::log2(beat.value)) - 2);
							for (int i = 0; i < flags; i++) {
								float yy = mid - 6 * S + i * 5 * S;
								B.circle(Role::Rest, bx - 3 * S, yy, 1.6f * S);
								B.path(Role::Rest, {M(bx - 3 * S, yy + 1.4f * S), Q(bx, yy + 2.4f * S, bx + 3 * S, yy - 0.6f * S)}, 1.2f);
							}
							B.path(Role::Rest, {M(bx + 3 * S, mid - 6.5f * S), L(bx - 1 * S, mid - 6 * S + flags * 5 * S + 4 * S)}, 1.2f);
						}
						for (int d = 0; d < std::min(beat.dots, 2) && !beat.fullBar; d++)
							B.circle(Role::Rest, bx + (7 + d * 4) * S, mid - 3 * S, 1.4f * S);
						continue;
					}
					if (vi == 0)
						rhythm.push_back({&beat, bx, b->index, false});
					// x where this beat's sound ends: the next beat of the voice, or the bar end
					float endX = out.bars[size_t(b->index)].x1 - 4 * S;
					if (k + 1 < voice.size()) {
						auto nx = b->slotX.find(voice[k + 1].start);
						if (nx != b->slotX.end())
							endX = nx->second;
					}
					// lowest and highest string this beat plays, for the brush arrow
					int sMin = strings, sMax = -1;
					for (auto &n : beat.notes)
						if (n.string < strings)
							sMin = std::min(sMin, n.string), sMax = std::max(sMax, n.string);
					float beatW = 0; // widest fret number of the beat
					for (auto &n : beat.notes) {
						if (n.string >= strings)
							continue;
						std::string label = noteLabel(n);
						float ny = sy(n.string);
						BendRun *run = vi == 0 && o.techniques ? &bendRun[size_t(n.string)] : nullptr;
						const bool continued = run && n.tie && run->active;
						// Like Guitar Pro: a tied note is written again, in brackets, only at the start of a bar,
						// and not while it only carries a bend on.
						const bool hidden = continued || (n.tie && k > 0);
						float w = hidden ? 0 : label.size() * digitW * (n.tie ? 0.85f : 1) + 4 * S;
						beatW = std::max(beatW, w);
						if (!hidden) {
							gaps[size_t(n.string)].push_back({bx - w / 2, bx + w / 2});
							auto &t = B.text((n.tie || n.ghost || vi) ? Role::NoteSoft : Role::Note, bx, ny, label, (n.tie || n.ghost) ? font * 0.92f : font);
							t.bold = !(n.tie || n.ghost);
							t.string = n.string;
							if (vi == 0)
								t.beatKey = key;
							if (auto suffix = noteSuffix(n); !suffix.empty()) {
								float sw = suffix.size() * digitW * 0.7f + 2 * S;
								gaps[size_t(n.string)].push_back({bx + w / 2, bx + w / 2 + sw});
								B.text(Role::NoteSoft, bx + w / 2 + 1 * S, ny, suffix, font * 0.7f, 0).string = n.string;
							}
						}
						if (!o.techniques)
							continue;
						if (n.grace) {
							// grace note: a small fret just before the note, joined by a slur or a slide
							std::string gl = n.grace->dead ? "x" : std::to_string(n.grace->fret);
							float gw = gl.size() * digitW * 0.75f + 2 * S;
							float gx = bx - std::max(w, 8 * S) / 2 - 2 * S - gw / 2;
							gaps[size_t(n.string)].push_back({gx - gw / 2, gx + gw / 2});
							B.text(Role::NoteSoft, gx, ny, gl, font * 0.72f).string = n.string;
							if (n.grace->slide) {
								bool up = n.fret >= n.grace->fret;
								B.line(Role::Technique, gx + gw / 2, ny + (up ? 2 : -2) * S, bx - std::max(w, 8 * S) / 2, ny + (up ? -2 : 2) * S, 1.1f);
							}
							if (n.grace->hammer || n.grace->slide) {
								float ay = ny - gap * 0.5f;
								B.path(Role::Technique, {M(gx, ay), Q((gx + bx) / 2, ay - 5 * S, bx, ay)}, 1);
							}
						}
						// next note on this string (same row)
						const Note *next = nullptr;
						float nx = 0;
						for (size_t j = k + 1; j < voice.size() && !next; j++) {
							for (auto &m : voice[j].notes)
								if (m.string == n.string) { next = &m; nx = b->slotX[voice[j].start]; }
							if (!next && !voice[j].notes.empty())
								break;
						}
						if (!next && b->index + 1 < int(bars.size())) {
							auto &nb = bars[size_t(b->index + 1)];
							if (nb.sys == int(si) && !nb.bar->voices.empty() && !nb.bar->voices[0].empty())
								for (auto &m : nb.bar->voices[0][0].notes)
									if (m.string == n.string) { next = &m; nx = nb.slotX[nb.bar->voices[0][0].start]; }
						}
						if ((n.hammer || n.slide == Slide::Legato) && next) {
							float ay = ny - gap * 0.55f;
							B.path(Role::Technique, {M(bx + 3 * S, ay), Q((bx + nx) / 2, ay - 8 * S, nx - 3 * S, ay)});
							if (n.hammer)
								B.text(Role::SmallText, (bx + nx) / 2, ay - 7 * S, next->fret >= n.fret ? "H" : "P", font * 0.68f).bold = true;
						}
						if ((n.slide == Slide::Shift || n.slide == Slide::Legato) && next) {
							bool up = next->fret >= n.fret;
							float x1 = bx + w / 2 + 1 * S;
							float x2 = nx - (std::to_string(next->fret).size() * digitW + 4 * S) / 2 - 1 * S;
							B.line(Role::Technique, x1, ny + (up ? 3 : -3) * S, x2, ny + (up ? -3 : 3) * S, 1.3f);
						} else if (n.slide == Slide::OutDown || n.slide == Slide::OutUp) {
							float d = n.slide == Slide::OutUp ? -1.f : 1.f;
							B.line(Role::Technique, bx + w / 2, ny - d * 3 * S, bx + w / 2 + 10 * S, ny + d * 3 * S, 1.3f);
						}
						if (n.slideIn != SlideIn::None) {
							float d = n.slideIn == SlideIn::Below ? 1.f : -1.f;
							B.line(Role::Technique, bx - w / 2 - 10 * S, ny + d * 3 * S, bx - w / 2, ny - d * 3 * S, 1.3f);
						}
						if (run && !n.bend.empty()) {
							// Guitar Pro style: rises end in an arrow above the staff with the amount,
							// holds are a dashed line up there, releases come back down to the note.
							auto pts = bendShape(n.bend);
							float level = continued ? run->level : 0;
							float px = continued ? run->x : bx + w / 2;
							float py = level > 0 ? bendTop : ny - (continued ? 6 * S : 0);
							const float span = std::max(10 * S, endX - (bx + w / 2) - 3 * S);
							const float xStart = continued ? bx : bx + w / 2;
							auto X = [&](float t) { return xStart + std::max(6 * S, t * span); };
							auto arrowUp = [&](float ax, float ay) { B.path(Role::Arrow, {M(ax - 3 * S, ay + 5 * S), L(ax, ay), L(ax + 3 * S, ay + 5 * S), Z()}); };
							auto arrowDown = [&](float ax, float ay) { B.path(Role::Arrow, {M(ax - 3 * S, ay - 5 * S), L(ax, ay), L(ax + 3 * S, ay - 5 * S), Z()}); };
							auto amount = [&](float ax, float v) { B.text(Role::SmallText, ax, bendTop - 5 * S, bendLabel(v), font * 0.68f).bold = true; };
							for (size_t pi = 0; pi < pts.size(); pi++) {
								const float v = pts[pi].semis;
								if (pi == 0) {
									if (v > level + 0.01f && !continued) {
										// pre-bend: straight up from the note
										B.path(Role::Technique, {M(bx, ny - 7 * S), L(bx, bendTop + 4 * S)});
										arrowUp(bx, bendTop);
										amount(bx, v);
										px = bx, py = bendTop;
									} else if (std::abs(v - level) > 0.01f && continued) {
										// the bend changed pitch across the tie: treat as a move from here
										level = v;
										py = v > 0 ? bendTop : ny - 6 * S;
									}
									level = v;
									continue;
								}
								float x1 = std::max(X(pts[pi].t), px + 6 * S);
								if (v > level) {
									B.path(Role::Technique, {M(px, py), Q(x1, py, x1, bendTop + 4 * S)});
									arrowUp(x1, bendTop);
									amount(x1, v);
									py = bendTop;
								} else if (v < level) {
									float y1 = v > 0 ? (bendTop + ny) / 2 : ny - 6 * S;
									B.path(Role::Technique, {M(px, py), Q(x1, py, x1, y1 - 4 * S)});
									arrowDown(x1, y1);
									if (v > 0)
										B.text(Role::SmallText, x1 + 3 * S, y1 - 2 * S, bendLabel(v), font * 0.62f, 0).bold = true;
									py = y1;
								} else if (v > 0) {
									B.path(Role::Dashed, {M(px, bendTop + 1 * S), L(x1, bendTop + 1 * S)}, 1);
								}
								px = x1, level = v;
							}
							run->active = true;
							run->x = px;
							run->level = level;
						} else if (run && continued && run->level > 0) {
							// tied note without its own bend points: the pitch holds
							B.path(Role::Dashed, {M(run->x, bendTop + 1 * S), L(endX - 3 * S, bendTop + 1 * S)}, 1);
							run->x = endX - 3 * S;
						} else if (run && !n.tie) {
							run->active = false;
						}
					}
					if (!o.techniques || vi != 0)
						continue;
					auto has = [&](auto pred) { return std::any_of(beat.notes.begin(), beat.notes.end(), pred); };
					auto laneY = [&](float row) { return y + row + 6.5f * S; };
					const float effEnd = std::max(bx + 12 * S, endX - 4 * S); // where a beat-long effect line ends
					if (has([](auto &n) { return n.palmMute; }))
						addSpan(pmSpans, b->index, int(k), bx, int(voice.size()));
					if (has([](auto &n) { return n.letRing; }))
						addSpan(ringSpans, b->index, int(k), bx, int(voice.size()));
					for (auto &n : beat.notes)
						if (n.harmonic != Harmonic::None) {
							if (!harmTypes.empty() && harmTypes.back() != n.harmonic)
								harmSpans.push_back({b->index, int(k), int(voice.size()), bx, bx}); // a new kind starts its own label
							else
								addSpan(harmSpans, b->index, int(k), bx, int(voice.size()));
							if (harmTypes.size() < harmSpans.size())
								harmTypes.push_back(n.harmonic);
							break;
						}
					if (beat.brush != Stroke::None && sMax >= 0) {
						// strum: an arrow beside the chord, pointing the way the pick travels on the tab
						float ax = bx - std::max(beatW, 8 * S) / 2 - 5 * S;
						float y0 = sy(sMin) - 5 * S, y1 = sy(sMax) + 5 * S;
						B.line(Role::Technique, ax, y0 + (beat.brush == Stroke::Down ? 5 : 0) * S, ax, y1 - (beat.brush == Stroke::Up ? 5 : 0) * S, 1.2f);
						if (beat.brush == Stroke::Down)
							B.path(Role::Arrow, {M(ax - 3 * S, y0 + 6 * S), L(ax, y0), L(ax + 3 * S, y0 + 6 * S), Z()});
						else
							B.path(Role::Arrow, {M(ax - 3 * S, y1 - 6 * S), L(ax, y1), L(ax + 3 * S, y1 - 6 * S), Z()});
					}
					const bool wide = beat.vibrato || has([](auto &n) { return n.wideVibrato; });
					if (wide || has([](auto &n) { return n.vibrato; })) {
						// a wavy line as long as the note; wide vibrato is taller and slower
						float vy = laneY(rowVib), amp = (wide ? 6.f : 4.f) * S, step = (wide ? 6.f : 4.f) * S;
						std::vector<PathOp> ops{M(bx - 2 * S, vy)};
						int i = 0;
						for (float cx = bx - 2 * S; cx + step <= effEnd + 0.5f && i < 256; cx += step, i++)
							ops.push_back(Q(cx + step / 2, vy + (i % 2 ? amp : -amp) / 2, cx + step, vy));
						B.path(Role::Technique, std::move(ops), wide ? 1.5f : 1.2f);
					}
					if (has([](auto &n) { return n.trill >= 0; })) {
						float ty = laneY(rowTrill);
						auto &t = B.text(Role::SmallText, bx, ty, "tr", font * 0.75f);
						t.bold = t.italic = true;
						std::vector<PathOp> ops{M(bx + 7 * S, ty)};
						int i = 0;
						for (float cx = bx + 7 * S; cx + 3 * S <= effEnd + 0.5f && i < 256; cx += 3 * S, i++)
							ops.push_back(Q(cx + 1.5f * S, ty + (i % 2 ? 2.5f : -2.5f) * S, cx + 3 * S, ty));
						if (ops.size() > 1)
							B.path(Role::Technique, std::move(ops), 1);
					}
					if (beat.tremolo > 0 && !beat.rest) {
						// tremolo picking: one slash per beam (8th, 16th, 32nd)
						int n = beat.tremolo <= 8 ? 1 : beat.tremolo <= 16 ? 2 : 3;
						float c = laneY(rowTrem);
						for (int i = 0; i < n; i++) {
							float oy = (i - (n - 1) / 2.f) * 3 * S;
							B.line(Role::Technique, bx - 3.5f * S, c + oy + 1.5f * S, bx + 3.5f * S, c + oy - 1.5f * S, 1.8f);
						}
					}
					if (!beat.whammy.empty()) {
						// tremolo bar: the pitch drawn as a line around a centre, amounts in tones
						const float c = y + rowWhammy + 15 * S, span = effEnd - bx, unit = 2.2f * S;
						const bool runStart = k == 0 || voice[k - 1].whammy.empty();
						if (runStart)
							B.text(Role::SmallText, bx - 4 * S, y + rowWhammy + 4 * S, "w/bar", font * 0.65f, 0).italic = true;
						std::vector<PathOp> ops;
						float lastV = 1e9f;
						for (auto &p : beat.whammy) {
							float px = bx + p.t * span, py = c - std::clamp(p.semis * unit, -9 * S, 9 * S);
							ops.push_back(ops.empty() ? M(px, py) : L(px, py));
							if (std::abs(p.semis - lastV) > 0.01f && p.semis != 0)
								B.text(Role::SmallText, px + 2 * S, py + (p.semis < 0 ? 5 : -5) * S, whammyLabel(p.semis), font * 0.6f, 0).bold = true;
							lastV = p.semis;
						}
						B.path(Role::Technique, std::move(ops), 1.3f);
					}
					if (o.techniques && hasDyn && !beat.notes.empty()) {
						int d = 0;
						for (auto &n : beat.notes)
							d = std::max(d, n.dynamic >= 1 ? n.dynamic : defaultDyn);
						if (d != dynBefore) {
							auto &t = B.text(Role::SmallText, bx, dynY, dynamicName(d), font * 0.8f);
							t.bold = t.italic = true;
						}
						dynBefore = d;
					}
					if (beat.fadeIn) {
						float c = laneY(rowFade);
						B.path(Role::Technique, {M(effEnd, c - 3.5f * S), L(bx - 2 * S, c), L(effEnd, c + 3.5f * S)});
					}
					if (beat.technique)
						B.text(Role::SmallText, bx, laneY(rowTSP), beat.technique == 1 ? "T" : beat.technique == 2 ? "S" : "P", font * 0.75f).bold = true;
					if (beat.pickStroke != Stroke::None) {
						float pY = y + rowPick + 10.5f * S;
						if (beat.pickStroke == Stroke::Down)
							B.path(Role::Technique, {M(bx - 4 * S, pY), L(bx - 4 * S, pY - 7 * S), L(bx + 4 * S, pY - 7 * S), L(bx + 4 * S, pY)});
						else
							B.path(Role::Technique, {M(bx - 3.5f * S, pY - 8 * S), L(bx, pY), L(bx + 3.5f * S, pY - 8 * S)});
					}
					int accent = 0;
					for (auto &n : beat.notes)
						accent = std::max(accent, n.accent);
					const bool staccato = has([](auto &n) { return n.staccato; });
					if (accent || staccato) {
						float c = laneY(rowAccent);
						if (accent == 2) // heavy accent
							B.path(Role::Technique, {M(bx - 3.5f * S, c + 3 * S), L(bx, c - 3.5f * S), L(bx + 3.5f * S, c + 3 * S)}, 1.4f);
						else if (accent == 1)
							B.path(Role::Technique, {M(bx - 4 * S, c - 2.5f * S), L(bx + 4 * S, c), L(bx - 4 * S, c + 2.5f * S)}, 1.3f);
						if (staccato)
							B.circle(Role::Arrow, accent ? bx + 7 * S : bx, c, 1.4f * S);
					}
				}
			}
		}

		auto spans = [&](const std::vector<Span> &list, float row, auto labelOf) {
			const float sY = y + row + 9 * S;
			for (size_t i = 0; i < list.size(); i++) {
				auto &sp = list[i];
				std::string label = labelOf(i);
				float lw = float(label.size()) * 5.2f * S;
				B.text(Role::SmallText, sp.x0 - 4 * S, sY, label, font * 0.7f, 0).italic = true;
				if (sp.x1 > sp.x0 + lw)
					B.path(Role::Dashed, {M(sp.x0 + lw, sY - 3 * S), L(sp.x1 + 6 * S, sY - 3 * S), L(sp.x1 + 6 * S, sY + 1 * S)}, 1);
			}
		};
		spans(pmSpans, rowPM, [](size_t) { return std::string("P.M."); });
		spans(ringSpans, rowRing, [](size_t) { return std::string("let ring"); });
		spans(harmSpans, rowHarm, [&](size_t i) { return std::string(harmonicName(i < harmTypes.size() ? harmTypes[i] : Harmonic::Natural)); });

		if (o.rhythm) {
			const float stemTop = rhythmTop, stemBottom = rhythmTop + 22 * S, beamGap = 4.5f * S;
			auto levels = [](int v) { return std::max(0, int(std::log2(v)) - 2); };
			std::vector<std::vector<const RhythmItem *>> groups;
			std::string curId;
			bool open = false;
			for (auto &it : rhythm) {
				const auto &mb = *bars[size_t(it.bar)].mb;
				int groupTicks = mb.den == 8 ? kQuarter * 3 / 2 : kQuarter;
				std::string gid = std::to_string(it.bar) + ":" + std::to_string(it.beat->start / groupTicks);
				bool beamable = !it.rest && it.beat->value >= 8;
				// the file's own beaming wins over the automatic grouping by beat
				const int forced = it.beat->beam;
				bool sameBar = !groups.empty() && groups.back().back()->bar == it.bar;
				if (beamable && open && forced != 1 && (gid == curId || (forced == 2 && sameBar)))
					groups.back().push_back(&it);
				else {
					open = beamable;
					curId = gid;
					if (beamable)
						groups.push_back({&it});
				}
				if (it.rest || it.beat->value == 1)
					continue;
				float y0 = it.beat->value == 2 ? stemTop + 11 * S : stemTop;
				B.line(Role::Stem, it.x, y0, it.x, stemBottom, 1.2f);
				for (int d = 0; d < std::min(it.beat->dots, 2); d++)
					B.circle(Role::Beam, it.x + (4 + d * 4) * S, stemBottom - 2 * S, 1.4f * S);
			}
			for (auto &g : groups) {
				if (g.size() == 1) {
					auto *it = g[0];
					for (int l = 0; l < levels(it->beat->value); l++) {
						float yy = stemBottom - l * beamGap;
						B.path(Role::Stem, {M(it->x, yy), Q(it->x + 5 * S, yy - 3 * S, it->x + 7 * S, yy - 8 * S)});
					}
					continue;
				}
				int maxL = 0;
				for (auto *it : g)
					maxL = std::max(maxL, levels(it->beat->value));
				for (int l = 0; l < maxL; l++) {
					float yy = stemBottom - l * beamGap;
					for (size_t i = 0; i < g.size(); i++) {
						auto *a = g[i];
						if (levels(a->beat->value) <= l)
							continue;
						auto *nb = i + 1 < g.size() ? g[i + 1] : nullptr;
						if (nb && l > 0 && nb->beat->beam == 3)
							nb = nullptr; // secondary beams broken here
						if (nb && levels(nb->beat->value) > l) {
							B.rect(Role::Beam, a->x - 0.6f * S, yy - 2.6f * S, nb->x - a->x + 1.2f * S, 2.6f * S);
						} else {
							auto *p = i ? g[i - 1] : nullptr;
							if (p && l > 0 && a->beat->beam == 3)
								p = nullptr;
							if (!(p && levels(p->beat->value) > l)) {
								float bw = 6 * S;
								B.rect(Role::Beam, nb ? a->x : a->x - bw, yy - 2.6f * S, bw, 2.6f * S);
							}
						}
					}
				}
			}
			// tuplets: the number under each complete group; a bracket too when the group is not one beam
			std::map<const RhythmItem *, size_t> groupOf;
			for (size_t gi = 0; gi < groups.size(); gi++)
				for (auto *it : groups[gi])
					groupOf[it] = gi;
			for (size_t i = 0; i < rhythm.size();) {
				const RhythmItem &first = rhythm[i];
				const Beat &fb = *first.beat;
				if (fb.tupletNum < 2 || fb.tupletDen < 1) {
					i++;
					continue;
				}
				const int span = beatTicks(fb.value, 0, 0, 0) * fb.tupletDen;
				int sum = 0;
				size_t j = i;
				while (j < rhythm.size() && sum < span && rhythm[j].bar == first.bar && rhythm[j].beat->tupletNum == fb.tupletNum && rhythm[j].beat->tupletDen == fb.tupletDen)
					sum += rhythm[j++].beat->ticks;
				const RhythmItem &last = rhythm[j - 1];
				auto g0 = groupOf.find(&first);
				bool oneBeam = g0 != groupOf.end() && groups[g0->second].size() == j - i;
				for (size_t m = i; m < j && oneBeam; m++) {
					auto g = groupOf.find(&rhythm[m]);
					oneBeam = g != groupOf.end() && g->second == g0->second;
				}
				const float ty = stemBottom + 9 * S, mid = (first.x + last.x) / 2;
				B.text(Role::SmallText, mid, ty, std::to_string(fb.tupletNum), font * 0.68f).bold = true;
				if (!oneBeam && j - i > 1) {
					B.path(Role::Technique, {M(first.x - 3 * S, ty - 5 * S), L(first.x - 3 * S, ty), L(mid - 6 * S, ty)}, 1);
					B.path(Role::Technique, {M(mid + 6 * S, ty), L(last.x + 3 * S, ty), L(last.x + 3 * S, ty - 5 * S)}, 1);
				}
				i = j;
			}
		}

		// staff lines with gaps where notes sit, drawn underneath everything in this row
		const float xEnd = x;
		std::vector<Prim> lines;
		for (int s = 0; s < strings; s++) {
			auto &gs = gaps[size_t(s)];
			std::sort(gs.begin(), gs.end());
			float ly = sy(s), cur = lineStart;
			for (auto &[g0, g1] : gs) {
				if (g0 > cur) {
					Prim p;
					p.type = Prim::Line, p.role = Role::Staff, p.x0 = cur, p.y0 = ly, p.x1 = g0, p.y1 = ly, p.string = s;
					lines.push_back(p);
				}
				cur = std::max(cur, g1);
			}
			if (cur < xEnd) {
				Prim p;
				p.type = Prim::Line, p.role = Role::Staff, p.x0 = cur, p.y0 = ly, p.x1 = xEnd, p.y1 = ly, p.string = s;
				lines.push_back(p);
			}
		}
		out.prims.insert(out.prims.begin() + std::ptrdiff_t(staffStart), lines.begin(), lines.end());
		sg.height = bottom - y;
		sg.x1 = xEnd;
		out.systems.push_back(sg);
		maxWidth = std::max(maxWidth, xEnd + margin);
		y = bottom + 8 * S;
	}
	// whole pixels; float rounding (600.0001) must not add one
	out.width = std::ceil((o.horizontal ? maxWidth : std::max(o.width, maxWidth)) - 0.01f);
	out.height = std::ceil(y - 0.01f);
	return out;
}

} // namespace tab
