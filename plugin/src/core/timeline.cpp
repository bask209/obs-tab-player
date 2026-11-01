// SPDX-License-Identifier: Apache-2.0
#include "timeline.hpp"
#include "limits.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace tab {

std::vector<int> playOrder(const std::vector<MasterBar> &bars)
{
	std::vector<int> order;
	std::map<int, int> reached;
	int start = 0, pass = 1;
	bool jumped = false;
	for (int i = 0, guard = 0; i < int(bars.size()) && guard < 4 * limits::kMaxBarOccurrences && order.size() < size_t(limits::kMaxBarOccurrences); guard++) {
		const auto &mb = bars[size_t(i)];
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
		i++;
	}
	return order;
}

namespace {
constexpr double kMaxRing = 4.0;

struct TempoAt {
	double tick, bpm;
};
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
			if (n.letRing)
				last->ring = true;
			return;
		}
		int midi = std::clamp(open + n.fret, 0, 127);
		if (n.harmonic == Harmonic::Natural)
			midi = open + naturalHarmonicOffset(n.fret);
		else if (n.harmonic != Harmonic::None && n.harmonic != Harmonic::Semi)
			midi += n.harmonic == Harmonic::Tap ? (n.harmonicFret ? n.harmonicFret : 12) : 12;
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
		ev.bend = n.bend;
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
		auto secAt = [&](double tk) {
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
		double barStart = time;
		double barDur = secAt(len);
		for (auto &c : changes)
			bpm = c.bpm;
		tl.bars.push_back({bi, barStart, barStart + barDur, startBpm});

		bool compound = mb.den == 8 && mb.num % 3 == 0 && mb.num > 3;
		int clicks = compound ? mb.num / 3 : mb.num;
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
					double t0 = barStart + secAt(beat.start);
					double dur = secAt(beat.start + beat.ticks) - secAt(beat.start);
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
	const auto &mb = score.masterBars[size_t(bar->index)];
	bool compound = mb.den == 8 && mb.num % 3 == 0 && mb.num > 3;
	int count = compound ? mb.num / 3 : mb.num;
	double sec = barTicks(mb) * 60.0 / (bar->bpm * kQuarter);
	for (int k = 0; k < count; k++)
		ci.clicks.push_back({k * sec / count, k == 0});
	ci.dur = sec;
	return ci;
}

} // namespace tab
