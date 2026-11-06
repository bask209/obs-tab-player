// SPDX-License-Identifier: Apache-2.0
#include "timeline.hpp"
#include "limits.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace tab {

namespace {
int highestBit(uint32_t v)
{
	int h = -1;
	for (int k = 0; k < 32; k++)
		if (v & (1u << k))
			h = k;
	return h;
}

// The beats you would tap along to in a bar: dotted beats in compound meters (6/8, 9/8, 12/16),
// and never more than five a second, so 16/16 at a fast tempo counts quarters instead.
int tappedBeats(const MasterBar &mb, double barSeconds)
{
	int n = std::max(1, mb.num);
	if (mb.den >= 8 && n % 3 == 0 && n > 3)
		n /= 3;
	while (n > 1 && n % 2 == 0 && barSeconds / n < 0.2)
		n /= 2;
	return n;
}

} // namespace

std::vector<int> playOrder(const std::vector<MasterBar> &bars)
{
	auto target = [&](uint8_t bit) {
		for (size_t i = 0; i < bars.size(); i++)
			if (bars[i].targets & bit)
				return int(i);
		return -1;
	};
	std::vector<int> order;
	std::map<int, int> reached;
	std::vector<bool> taken(bars.size()); // D.C./D.S. jumps are taken once
	int start = 0, pass = 1;
	bool jumped = false, afterJump = false;
	enum { Through, ToCoda, ToDoubleCoda, ToFine } mode = Through;
	for (int i = 0, guard = 0; i < int(bars.size()) && guard < 4 * limits::kMaxBarOccurrences && order.size() < size_t(limits::kMaxBarOccurrences); guard++) {
		const auto &mb = bars[size_t(i)];
		if (afterJump) {
			// after a D.C. or D.S. the repeats are not played again, and of the endings only the last
			if (mb.alternate) {
				bool later = false;
				for (size_t j = size_t(i) + 1; j < bars.size() && bars[j].alternate; j++)
					later = later || highestBit(bars[j].alternate) > highestBit(mb.alternate);
				if (later) {
					i++;
					continue;
				}
			}
			order.push_back(i);
		} else {
			if (mb.repeatStart && !jumped) {
				start = i;
				pass = 1;
			}
			jumped = false;
			if (mb.alternate && !(mb.alternate & (1u << (pass - 1)))) {
				i++;
				continue;
			}
			order.push_back(i);
			if (mb.repeatPlays > 1) {
				int n = reached.count(i) ? reached[i] : 1;
				if (n < mb.repeatPlays) {
					reached[i] = n + 1;
					pass = n + 1;
					i = start;
					jumped = true;
					continue;
				}
				reached.erase(i);
				start = i + 1;
			}
		}
		if (mode == ToFine && (mb.targets & kFine))
			break;
		if ((mode == ToCoda && mb.jump == Jump::DaCoda) || (mode == ToDoubleCoda && mb.jump == Jump::DaDoubleCoda)) {
			int to = target(mode == ToCoda ? kCoda : kDoubleCoda);
			if (to > i) {
				mode = Through;
				i = to;
				continue;
			}
		}
		const int j = int(mb.jump);
		if (j >= int(Jump::DaCapo) && j <= int(Jump::DaSegnoSegnoAlFine) && !taken[size_t(i)]) {
			taken[size_t(i)] = true;
			const int kind = (j - 1) / 4, al = (j - 1) % 4; // from: capo, segno, segno segno; al: -, coda, double coda, fine
			int to = kind == 0 ? 0 : target(kind == 1 ? kSegno : kSegnoSegno);
			if (to >= 0) {
				mode = al == 1 ? ToCoda : al == 2 ? ToDoubleCoda : al == 3 ? ToFine : Through;
				afterJump = true;
				i = to;
				continue;
			}
		}
		i++;
	}
	return order;
}

namespace {
constexpr double kMaxRing = 4.0;

struct TempoAt {
	double tick, bpm;
};

// Swing ("triplet feel"): Guitar Pro plays a pair of 8ths (or 16ths) inside one beat unevenly,
// e.g. as a triplet quarter + eighth. Only beats forming such a pair move; the rest stay straight.
std::pair<int, int> feelTicks(const Voice &v, size_t k, Feel feel)
{
	const Beat &b = v[k];
	const int s = b.start, e = b.start + b.ticks;
	if (feel == Feel::None || b.tupletNum)
		return {s, e};
	const bool sixteenth = feel == Feel::Triplet16th || feel == Feel::Dotted16th || feel == Feel::Scottish16th;
	const bool triplet = feel == Feel::Triplet8th || feel == Feel::Triplet16th;
	const int unit = sixteenth ? kQuarter / 4 : kQuarter / 2, group = unit * 2;
	const int first = triplet ? group * 2 / 3 : (feel == Feel::Dotted8th || feel == Feel::Dotted16th) ? group * 3 / 4 : group / 4;
	const int off = s % group, base = s - off;
	auto plain = [&](size_t j, int start, int ticks) { return j < v.size() && v[j].start == start && v[j].ticks == ticks && !v[j].tupletNum; };
	// two equal halves, or (triplet feel) a dotted half + the short rest of the group
	const int split[2] = {unit, unit * 3 / 2};
	for (int sp : split) {
		if (sp != unit && !triplet)
			break;
		if (off == 0 && b.ticks == sp && plain(k + 1, s + sp, group - sp))
			return {s, s + first};
		if (off == sp && b.ticks == group - sp && k > 0 && plain(k - 1, base, sp))
			return {base + first, base + group};
	}
	return {s, e};
}
} // namespace

Timeline buildTimeline(const Score &score, int displayTrack)
{
	Timeline tl;
	tl.order = playOrder(score.masterBars);
	double bpm = std::clamp(score.tempo > 0 ? score.tempo : 120.0, limits::kMinBpm, limits::kMaxBpm);
	double time = 0;

	std::vector<std::vector<TempoAt>> beatTempos(score.masterBars.size());
	for (auto &track : score.tracks)
		for (size_t bi = 0; bi < track.bars.size() && bi < beatTempos.size(); bi++)
			for (auto &v : track.bars[bi].voices)
				for (auto &b : v)
					if (b.tempo > 0)
						beatTempos[bi].push_back({double(b.start), b.tempo});

	std::vector<std::vector<size_t>> lastIndex(score.tracks.size(), std::vector<size_t>(16, SIZE_MAX));
	tl.notes.reserve(4096);

	auto addNote = [&](const Track &track, int ti, const Beat &beat, const Note &n, double t0, double dur) {
		if (n.string < 0 || n.string >= 16 || tl.notes.size() >= limits::kMaxNoteEvents)
			return;
		int open = (n.string < int(track.tuning.size()) ? track.tuning[size_t(n.string)] : 40) + track.capo;
		size_t li = lastIndex[size_t(ti)][size_t(n.string)];
		NoteEvent *last = li == SIZE_MAX ? nullptr : &tl.notes[li];
		if (n.tie && last) {
		last->dur = std::max(last->dur, t0 + dur - last->t);
		// a bend (or whammy) can carry on through tied notes (bend, hold, release): extend the curve
		for (auto &p : n.bend.empty() ? beat.whammy : n.bend)
				if (last->bend.size() < 64)
					last->bend.push_back({float(t0 - last->t + p.t * dur), p.semis});
			if (n.letRing)
				last->ring = true;
			return;
		}
		int midi = std::clamp(open + n.fret, 0, 127);
		if (n.harmonic == Harmonic::Natural)
			midi = open + naturalHarmonicOffset(n.harmonicFret > 0 ? n.harmonicFret : n.fret);
		else if (n.harmonic != Harmonic::None && n.harmonic != Harmonic::Semi)
			midi += naturalHarmonicOffset(n.harmonicFret > 0 ? n.harmonicFret : 12); // touched that many frets above the note
		int dyn = n.dynamic >= 0 ? n.dynamic : 6;
		float vel = 0.35f + dyn * 0.08f;
		if (n.ghost)
			vel *= 0.55f;
		if (n.accent)
			vel = std::min(1.15f, vel * (n.accent == 2 ? 1.35f : 1.2f));
		NoteEvent ev;
		ev.t = t0;
		ev.dur = dur;
		ev.midi = midi;
		ev.vel = vel;
		ev.track = ti;
		ev.string = n.string;
		ev.kind = n.dead ? NoteKind::Dead : n.palmMute ? NoteKind::Muted : n.harmonic != Harmonic::None ? NoteKind::Harmonic : NoteKind::Pluck;
		if (n.staccato)
			ev.dur = dur * 0.5;
		if (n.palmMute)
			ev.dur = std::min(dur, 0.35);
		ev.ring = n.letRing || beat.letRing;
		ev.bend = n.bend.empty() ? beat.whammy : n.bend; // the whammy bar moves every note of the beat
		for (auto &p : ev.bend)
			p.t = float(p.t * dur); // seconds from the note start (at speed 1)
		ev.vibrato = n.vibrato || beat.vibrato;
		if (last && (last->hammer || last->slideLegato))
			ev.legato = true;
		ev.hammer = n.hammer;
		if (n.slide == Slide::Shift || n.slide == Slide::Legato) {
			ev.slideNext = true;
			ev.slideLegato = n.slide == Slide::Legato;
		} else if (n.slide == Slide::OutDown) {
			ev.slideOut = -5;
		} else if (n.slide == Slide::OutUp) {
			ev.slideOut = 5;
		}
		if (n.slideIn != SlideIn::None)
			ev.slideIn = n.slideIn == SlideIn::Below ? -3.f : 3.f;
		if (last && last->slideNext && last->slideTarget < 0)
			last->slideTarget = midi;
		if (n.grace) {
			double gd = std::min(0.06, dur / 3);
			NoteEvent g;
			g.t = n.grace->onBeat ? t0 : std::max(0.0, t0 - gd);
			g.dur = gd;
			g.midi = open + n.grace->fret;
			g.vel = vel * 0.8f;
			g.track = ti;
			g.string = n.string;
			g.kind = n.grace->dead ? NoteKind::Dead : NoteKind::Pluck;
			if (n.grace->slide)
				g.slideTarget = midi;
			tl.notes.push_back(g);
			if (n.grace->hammer || n.grace->slide)
				ev.legato = true;
			if (n.grace->onBeat)
				ev.t = t0 + gd;
		}
		if (beat.brush != Stroke::None) {
			int order = beat.brush == Stroke::Down ? int(track.tuning.size()) - 1 - n.string : n.string;
			ev.t += order * 0.006;
		}
		if (beat.tremolo >= 8 && beat.tremolo <= 64 && beat.ticks > 0) {
			double step = (4.0 / beat.tremolo) * (dur / (double(beat.ticks) / kQuarter));
			for (double tt = t0; step > 1e-3 && tt < t0 + dur - step / 2 && tl.notes.size() < limits::kMaxNoteEvents; tt += step) {
				NoteEvent e = ev;
				e.t = tt;
				e.dur = step;
				tl.notes.push_back(e);
			}
		} else if (n.trill >= 0 && n.trillSpeed >= 16 && n.trillSpeed <= 64 && beat.ticks > 0) {
			// trill: the note and the trill fret alternate, slurred
			double step = (4.0 / n.trillSpeed) * (dur / (double(beat.ticks) / kQuarter));
			int k = 0;
			for (double tt = t0; step > 1e-3 && tt < t0 + dur - step / 2 && tl.notes.size() < limits::kMaxNoteEvents; tt += step, k++) {
				NoteEvent e = ev;
				e.t = tt;
				e.dur = step;
				if (k % 2)
					e.midi = std::clamp(open + n.trill, 0, 127);
				if (k)
					e.legato = true;
				tl.notes.push_back(e);
			}
		} else {
			tl.notes.push_back(ev);
		}
		lastIndex[size_t(ti)][size_t(n.string)] = tl.notes.size() - 1;
	};

	for (size_t occ = 0; occ < tl.order.size(); occ++) {
		int bi = tl.order[occ];
		const auto &mb = score.masterBars[size_t(bi)];
		int len = barTicks(mb);
		std::vector<TempoAt> changes = beatTempos[size_t(bi)];
		for (auto &c : mb.tempoChanges)
			changes.push_back({std::clamp(c.ratio, 0.0, 1.0) * len, c.bpm});
		for (auto &c : changes)
			c.bpm = std::clamp(c.bpm, limits::kMinBpm, limits::kMaxBpm);
		std::sort(changes.begin(), changes.end(), [](auto &a, auto &b) { return a.tick < b.tick; });
		double startBpm = bpm;
		auto baseSec = [&](double tk) {
			double t = 0, last = 0, cur = startBpm;
			for (auto &c : changes) {
				if (c.tick >= tk)
					break;
				t += (c.tick - last) * 60.0 / (cur * kQuarter);
				last = c.tick;
				cur = c.bpm;
			}
			return t + (tk - last) * 60.0 / (cur * kQuarter);
		};
		// fermatas hold the beat they sit on longer: its time is stretched by their length
		struct Hold {
			double from, to, length;
		};
		std::vector<Hold> holds;
		for (auto &f : mb.fermatas) {
			double tk = f.ratio * len, from = tk, to = tk + kQuarter;
			for (auto &track : score.tracks)
				if (size_t(bi) < track.bars.size() && !track.bars[size_t(bi)].voices.empty()) {
					for (auto &b : track.bars[size_t(bi)].voices[0])
						if (b.start <= tk && tk < b.start + b.ticks)
							from = b.start, to = b.start + b.ticks;
					break;
				}
			holds.push_back({from, std::min(to, double(len)), f.length});
		}
		auto secAt = [&](double tk) {
			double t = baseSec(tk);
			for (auto &h : holds)
				if (tk > h.from && h.to > h.from)
					t += (baseSec(std::min(tk, h.to)) - baseSec(h.from)) * h.length;
			return t;
		};
		double barStart = time;
		double barDur = secAt(len);
		for (auto &c : changes)
			bpm = c.bpm;
		tl.bars.push_back({bi, barStart, barStart + barDur, startBpm});

		int clicks = tappedBeats(mb, barDur);
		for (int k = 0; k < clicks; k++)
			tl.clicks.push_back({barStart + secAt(double(k) * len / clicks), k == 0});

		for (size_t ti = 0; ti < score.tracks.size(); ti++) {
			const auto &track = score.tracks[ti];
			if (size_t(bi) >= track.bars.size())
				continue;
			const auto &bar = track.bars[size_t(bi)];
			for (size_t vi = 0; vi < bar.voices.size(); vi++) {
				const auto &voice = bar.voices[vi];
				for (size_t k = 0; k < voice.size(); k++) {
					const auto &beat = voice[k];
					auto [from, to] = feelTicks(voice, k, mb.feel);
					double t0 = barStart + secAt(from);
					double dur = secAt(to) - secAt(from);
					if (int(ti) == displayTrack && vi == 0)
						tl.beats.push_back({t0, dur, bi, int(k), int(occ)});
					if (track.percussion || beat.rest)
						continue;
					for (auto &n : beat.notes)
						addNote(track, int(ti), beat, n, t0, dur);
				}
			}
		}
		time += barDur;
	}

	// Let-ring sustains until the next note on the same string; one sound per string.
	std::map<int, double> nextOnString;
	for (size_t i = tl.notes.size(); i-- > 0;) {
		auto &n = tl.notes[i];
		int key = n.track * 16 + n.string;
		auto it = nextOnString.find(key);
		double next = it == nextOnString.end() ? -1 : it->second;
		if (n.ring)
			n.dur = std::min(kMaxRing, (next >= 0 ? next : n.t + kMaxRing) - n.t);
		if (next >= 0 && n.t + n.dur > next)
			n.dur = std::max(0.01, next - n.t);
		nextOnString[key] = n.t;
	}
	std::stable_sort(tl.notes.begin(), tl.notes.end(), [](auto &a, auto &b) { return a.t < b.t; });
	tl.duration = time;
	return tl;
}

std::pair<double, double> loopRange(const Timeline &tl, int fromBar, int toBar)
{
	if (!fromBar && !toBar)
		return {0, tl.duration};
	int a = std::max(1, fromBar ? fromBar : 1) - 1;
	int b = std::max(a + 1, toBar ? toBar : fromBar) - 1;
	const BarSpan *first = nullptr;
	for (auto &x : tl.bars)
		if (x.index == a) {
			first = &x;
			break;
		}
	if (!first)
		return {0, tl.duration};
	const BarSpan *last = first;
	for (auto &x : tl.bars)
		if (x.index == b && x.start >= first->start) {
			last = &x;
			break;
		}
	return {first->start, last->end};
}

CountIn countInClicks(const Score &score, const Timeline &tl, double from)
{
	CountIn ci;
	if (tl.bars.empty())
		return ci;
	const BarSpan *bar = &tl.bars[0];
	for (auto &b : tl.bars)
		if (b.end > from + 1e-6) {
			bar = &b;
			break;
		}
	if (bar->index < 0 || size_t(bar->index) >= score.masterBars.size())
		return ci;
	// one bar of the meter and tempo of the bar you count into
	const auto &mb = score.masterBars[size_t(bar->index)];
	double sec = barTicks(mb) * 60.0 / (std::clamp(bar->bpm, limits::kMinBpm, limits::kMaxBpm) * kQuarter);
	if (!(sec > 0) || !std::isfinite(sec))
		return ci;
	int count = tappedBeats(mb, sec);
	for (int k = 0; k < count; k++)
		ci.clicks.push_back({k * sec / count, k == 0});
	ci.dur = sec;
	return ci;
}

} // namespace tab
