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

std::string noteLabel(const Note &n)
{
	if (n.dead)
		return "x";
	if (n.harmonic == Harmonic::Natural)
		return "<" + std::to_string(n.fret) + ">";
	if (n.harmonic == Harmonic::Artificial || n.harmonic == Harmonic::Pinch)
		return std::to_string(n.fret);
	if (n.ghost || n.tie)
		return "(" + std::to_string(n.fret) + ")";
	return std::to_string(n.fret);
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
				for (auto &n : beat.notes)
					digits = std::max(digits, noteLabel(n).size());
				float minW = digits * digitW + 7 * S;
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
		bool hasSpan = o.techniques && noteAny([](const Note &n) { return n.palmMute || n.letRing; });
		bool hasTech = o.techniques && (any([](const Beat &b) { return b.pickStroke != Stroke::None || b.vibrato; }) || noteAny([](const Note &n) { return n.vibrato; }));
		float top = 4 * S;
		float rowChord = top; if (hasChords) top += 16 * S;
		float rowText = top; if (hasText) top += 16 * S;
		float rowSpan = top; if (hasSpan) top += 13 * S;
		float rowTech = top; if (hasTech) top += 13 * S;
		if (hasBend) top += 18 * S;
		if (o.barNumbers && !hasText && !hasChords) top += 6 * S;
		const float staffTop = y + top + 6 * S, staffH = gap * (strings - 1), staffBottom = staffTop + staffH;
		const float rhythmTop = staffBottom + 7 * S;
		const float bottom = staffBottom + (o.rhythm ? 34 * S : 10 * S);
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
			if (!mb.marker.empty())
				B.text(Role::Marker, barX + 3 * S, y + rowText + 11 * S, mb.marker, font * 0.9f, 0).bold = true;
			if (mb.alternate) {
				std::string nums;
				for (int k = 0; k < 8; k++)
					if (mb.alternate & (1u << k))
						nums += (nums.empty() ? "" : ", ") + std::to_string(k + 1) + ".";
				float ay = y + rowText + 2 * S;
				B.path(Role::Technique, {M(barX + 2 * S, ay + 12 * S), L(barX + 2 * S, ay), L(barEnd - 4 * S, ay)});
				B.text(Role::SmallText, barX + 6 * S, ay + 10 * S, nums, font * 0.75f, 0);
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
		std::vector<Span> pmSpans, ringSpans;
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
						continue;
					}
					if (vi == 0)
						rhythm.push_back({&beat, bx, b->index, false});
					for (auto &n : beat.notes) {
						if (n.string >= strings)
							continue;
						std::string label = noteLabel(n);
						float ny = sy(n.string);
						float w = label.size() * digitW * (n.tie ? 0.85f : 1) + 4 * S;
						gaps[size_t(n.string)].push_back({bx - w / 2, bx + w / 2});
						auto &t = B.text((n.tie || n.ghost || vi) ? Role::NoteSoft : Role::Note, bx, ny, label, (n.tie || n.ghost) ? font * 0.92f : font);
						t.bold = !(n.tie || n.ghost);
						t.string = n.string;
						if (vi == 0)
							t.beatKey = key;
						if (!o.techniques)
							continue;
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
						if (n.hammer && next) {
							float ay = ny - gap * 0.55f;
							B.path(Role::Technique, {M(bx + 3 * S, ay), Q((bx + nx) / 2, ay - 8 * S, nx - 3 * S, ay)});
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
						if (!n.bend.empty()) {
							float peak = 0;
							for (auto &p : n.bend)
								peak = std::max(peak, p.semis);
							float first = n.bend.front().semis, last = n.bend.back().semis;
							float topY = staffTop - 8 * S, x1 = bx + w / 2, x2 = bx + std::max(14 * S, w / 2 + 10 * S);
							bool up = first < peak - 0.05f;
							if (up)
								B.path(Role::Technique, {M(x1, ny), Q(x2, ny, x2, topY + 4 * S)});
							else
								B.path(Role::Technique, {M(bx, ny - 6 * S), L(bx, topY + 4 * S)});
							float ax = up ? x2 : bx;
							B.path(Role::Arrow, {M(ax - 3 * S, topY + 5 * S), L(ax, topY), L(ax + 3 * S, topY + 5 * S), Z()});
							B.text(Role::SmallText, ax, topY - 5 * S, bendLabel(peak), font * 0.68f).bold = true;
							if (last < peak - 0.05f) {
								float rx = ax + 10 * S;
								B.path(Role::Technique, {M(ax + 1 * S, topY + 2 * S), Q(rx, topY + 2 * S, rx, ny - 4 * S)});
								B.path(Role::Arrow, {M(rx - 3 * S, ny - 9 * S), L(rx, ny - 4 * S), L(rx + 3 * S, ny - 9 * S), Z()});
							}
						}
						if (n.staccato)
							B.circle(Role::Arrow, bx, ny + 7 * S, 1.4f * S);
					}
					if (!o.techniques || vi != 0)
						continue;
					float tY = y + rowTech + 10 * S;
					if (std::any_of(beat.notes.begin(), beat.notes.end(), [](auto &n) { return n.palmMute; }))
						addSpan(pmSpans, b->index, int(k), bx, int(voice.size()));
					if (std::any_of(beat.notes.begin(), beat.notes.end(), [](auto &n) { return n.letRing; }))
						addSpan(ringSpans, b->index, int(k), bx, int(voice.size()));
					if (beat.vibrato || std::any_of(beat.notes.begin(), beat.notes.end(), [](auto &n) { return n.vibrato; })) {
						std::vector<PathOp> ops{M(bx - 3 * S, tY - 3 * S)};
						float step = 4 * S, cx = bx - 3 * S;
						for (int i = 0; i < 4; i++, cx += step)
							ops.push_back(Q(cx + step / 2, tY - 3 * S + (i % 2 ? 3 : -3) * S, cx + step, tY - 3 * S));
						B.path(Role::Technique, std::move(ops));
					}
					if (std::any_of(beat.notes.begin(), beat.notes.end(), [](auto &n) { return n.accent > 0; }))
						B.path(Role::Technique, {M(bx - 4 * S, staffTop - 9 * S), L(bx + 4 * S, staffTop - 6.5f * S), L(bx - 4 * S, staffTop - 4 * S)});
					if (beat.pickStroke == Stroke::Down)
						B.path(Role::Technique, {M(bx - 4 * S, tY), L(bx - 4 * S, tY - 7 * S), L(bx + 4 * S, tY - 7 * S), L(bx + 4 * S, tY)});
					else if (beat.pickStroke == Stroke::Up)
						B.path(Role::Technique, {M(bx - 3.5f * S, tY - 8 * S), L(bx, tY), L(bx + 3.5f * S, tY - 8 * S)});
				}
			}
		}

		float sY = y + rowSpan + 9 * S;
		auto spans = [&](const std::vector<Span> &list, const char *label) {
			float lw = float(std::char_traits<char>::length(label)) * 5.2f * S;
			for (auto &sp : list) {
				B.text(Role::SmallText, sp.x0 - 4 * S, sY, label, font * 0.7f, 0).italic = true;
				if (sp.x1 > sp.x0 + lw)
					B.path(Role::Dashed, {M(sp.x0 + lw, sY - 3 * S), L(sp.x1 + 6 * S, sY - 3 * S), L(sp.x1 + 6 * S, sY + 1 * S)}, 1);
			}
		};
		spans(pmSpans, "P.M.");
		spans(ringSpans, "let ring");

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
				if (beamable && open && gid == curId)
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
				if (it.beat->dots)
					B.circle(Role::Beam, it.x + 4 * S, stemBottom - 2 * S, 1.4f * S);
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
						if (nb && levels(nb->beat->value) > l) {
							B.rect(Role::Beam, a->x - 0.6f * S, yy - 2.6f * S, nb->x - a->x + 1.2f * S, 2.6f * S);
						} else {
							auto *p = i ? g[i - 1] : nullptr;
							if (!(p && levels(p->beat->value) > l)) {
								float bw = 6 * S;
								B.rect(Role::Beam, nb ? a->x : a->x - bw, yy - 2.6f * S, bw, 2.6f * S);
							}
						}
					}
				}
				int tup = g[0]->beat->tupletNum;
				if (tup > 1 && std::all_of(g.begin(), g.end(), [&](auto *it) { return it->beat->tupletNum == tup; }))
					B.text(Role::SmallText, (g.front()->x + g.back()->x) / 2, stemBottom + 9 * S, std::to_string(tup), font * 0.68f).bold = true;
			}
			for (auto &it : rhythm) {
				if (it.rest || it.beat->tupletNum < 2 || it.beat->value >= 8)
					continue;
				if (it.beat->start % (kQuarter * 2) == 0 || it.beat == rhythm.front().beat)
					B.text(Role::SmallText, it.x, stemBottom + 9 * S, std::to_string(it.beat->tupletNum), font * 0.68f).bold = true;
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
	out.width = o.horizontal ? std::ceil(maxWidth) : std::ceil(std::max(o.width, maxWidth));
	out.height = std::ceil(y);
	return out;
}

} // namespace tab
