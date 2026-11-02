// SPDX-License-Identifier: Apache-2.0
// Minimal XML reader and the GPIF (Guitar Pro 6/7/8) score reader.
#include "readers.hpp"
#include "limits.hpp"
#include "text.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace tab {
namespace {

struct XNode {
	std::string name, text;
	std::vector<std::pair<std::string, std::string>> attrs;
	std::vector<std::unique_ptr<XNode>> children;

	const XNode *child(std::string_view n) const
	{
		for (auto &c : children)
			if (c->name == n)
				return c.get();
		return nullptr;
	}
	std::string attr(std::string_view n) const
	{
		for (auto &a : attrs)
			if (a.first == n)
				return a.second;
		return {};
	}
	std::string childText(std::string_view n) const
	{
		auto c = child(n);
		return c ? trim(c->text) : std::string();
	}
};

std::string decodeEntities(std::string_view s)
{
	if (s.find('&') == std::string_view::npos)
		return std::string(s);
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] != '&') {
			out += s[i];
			continue;
		}
		size_t semi = s.find(';', i);
		if (semi == std::string_view::npos || semi - i > 10) {
			out += '&';
			continue;
		}
		auto e = s.substr(i + 1, semi - i - 1);
		if (e == "lt") out += '<';
		else if (e == "gt") out += '>';
		else if (e == "amp") out += '&';
		else if (e == "quot") out += '"';
		else if (e == "apos") out += '\'';
		else if (e.size() > 1 && e[0] == '#') {
			bool hex = e[1] == 'x' || e[1] == 'X';
			uint32_t c = 0;
			bool ok = e.size() > (hex ? 2u : 1u);
			for (size_t k = hex ? 2 : 1; k < e.size() && ok; k++) {
				char ch = e[k];
				int d = ch >= '0' && ch <= '9' ? ch - '0' : hex && ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : hex && ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
				ok = d >= 0 && c < 0x110000;
				c = c * (hex ? 16 : 10) + uint32_t(std::max(d, 0));
			}
			if (!ok || c == 0 || c >= 0x110000 || (c >= 0xD800 && c <= 0xDFFF))
				c = 0xFFFD;
			if (c < 0x80) out += char(c);
			else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
			else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
			else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
		} else {
			out.append(s.substr(i, semi - i + 1));
		}
		i = semi;
	}
	return out;
}

std::unique_ptr<XNode> parseXml(const std::string &src)
{
	auto root = std::make_unique<XNode>();
	std::vector<XNode *> stack{root.get()};
	size_t i = 0, n = src.size();
	while (i < n) {
		size_t lt = src.find('<', i);
		if (lt == std::string::npos)
			break;
		if (lt > i) {
			auto t = std::string_view(src).substr(i, lt - i);
			if (t.find_first_not_of(" \t\r\n") != std::string_view::npos)
				stack.back()->text += decodeEntities(t);
		}
		if (src.compare(lt, 9, "<![CDATA[") == 0) {
			size_t end = src.find("]]>", lt + 9);
			if (end == std::string::npos) end = n;
			stack.back()->text.append(src, lt + 9, end - lt - 9);
			i = end + 3;
		} else if (src.compare(lt, 4, "<!--") == 0) {
			size_t end = src.find("-->", lt + 4);
			i = end == std::string::npos ? n : end + 3;
		} else if (lt + 1 < n && (src[lt + 1] == '?' || src[lt + 1] == '!')) {
			size_t end = src.find('>', lt + 1);
			i = end == std::string::npos ? n : end + 1;
		} else if (lt + 1 < n && src[lt + 1] == '/') {
			size_t end = src.find('>', lt + 2);
			if (stack.size() > 1)
				stack.pop_back();
			i = end == std::string::npos ? n : end + 1;
		} else {
			size_t end = lt + 1;
			char q = 0;
			for (; end < n; end++) {
				char c = src[end];
				if (q) { if (c == q) q = 0; }
				else if (c == '"' || c == '\'') q = c;
				else if (c == '>') break;
			}
			bool selfClose = end > 0 && src[end - 1] == '/';
			std::string_view body(src.data() + lt + 1, (selfClose ? end - 1 : end) - lt - 1);
			auto node = std::make_unique<XNode>();
			size_t sp = body.find_first_of(" \t\r\n");
			node->name = std::string(body.substr(0, sp));
			if (sp != std::string_view::npos) {
				auto rest = body.substr(sp);
				size_t k = 0;
				while (k < rest.size()) {
					size_t eq = rest.find('=', k);
					if (eq == std::string_view::npos) break;
					auto key = trim(rest.substr(k, eq - k));
					size_t qs = rest.find_first_of("\"'", eq);
					if (qs == std::string_view::npos) break;
					size_t qe = rest.find(rest[qs], qs + 1);
					if (qe == std::string_view::npos) break;
					node->attrs.emplace_back(key, decodeEntities(rest.substr(qs + 1, qe - qs - 1)));
					k = qe + 1;
				}
			}
			XNode *raw = node.get();
			stack.back()->children.push_back(std::move(node));
			if (!selfClose) {
				if (stack.size() >= limits::kMaxXmlDepth)
					throw std::runtime_error("XML nested too deeply");
				stack.push_back(raw);
			}
			i = end + 1;
		}
	}
	return root;
}

std::vector<int> ids(const std::string &s)
{
	std::vector<int> out;
	std::istringstream in(s);
	int v;
	while (in >> v)
		out.push_back(v);
	return out;
}

double num(const std::string &s, double fallback = 0)
{
	try {
		double v = s.empty() ? fallback : std::stod(s);
		return std::isfinite(v) ? v : fallback; // "nan" and "inf" from a hostile file must not reach the layout
	} catch (...) {
		return fallback;
	}
}

using Props = std::unordered_map<std::string, const XNode *>;
Props props(const XNode *node)
{
	Props out;
	if (!node)
		return out;
	if (auto p = node->child("Properties"))
		for (auto &c : p->children)
			if (c->name == "Property")
				out[c->attr("name")] = c.get();
	return out;
}
std::string propText(const Props &p, const char *name, const char *inner)
{
	auto it = p.find(name);
	return it == p.end() ? std::string() : it->second->childText(inner);
}

} // namespace

Score readGpif(const std::string &xml, const std::string &format)
{
	auto doc = parseXml(xml);
	const XNode *gpif = doc->child("GPIF");
	if (!gpif)
		throw std::runtime_error("Not a GPIF document");
	Score score;
	score.format = format;
	if (auto s = gpif->child("Score")) {
		score.title = s->childText("Title");
		score.subTitle = s->childText("SubTitle");
		score.artist = s->childText("Artist");
		score.album = s->childText("Album");
		score.words = s->childText("Words");
		score.music = s->childText("Music");
		score.copyright = s->childText("Copyright");
		score.tab = s->childText("Tabber");
		score.instructions = s->childText("Instructions");
		score.notices = s->childText("Notices");
	}

	auto pool = [&](const char *list, const char *item) {
		std::unordered_map<std::string, const XNode *> m;
		if (auto l = gpif->child(list))
			for (auto &c : l->children)
				if (c->name == item)
					m[c->attr("id")] = c.get();
		return m;
	};
	auto barsById = pool("Bars", "Bar");
	auto voicesById = pool("Voices", "Voice");
	auto beatsById = pool("Beats", "Beat");
	auto notesById = pool("Notes", "Note");
	auto rhythmsById = pool("Rhythms", "Rhythm");

	struct Tempo { int bar; double ratio, bpm; };
	std::vector<Tempo> tempos;
	if (auto mt = gpif->child("MasterTrack"))
		if (auto au = mt->child("Automations"))
			for (auto &a : au->children) {
				if (a->name != "Automation" || a->childText("Type") != "Tempo")
					continue;
				std::istringstream in(a->childText("Value"));
				double bpm = 120;
				int unit = 2;
				in >> bpm >> unit;
				static const double unitQuarters[] = {1, 0.5, 1, 1.5, 2, 3};
				if (unit >= 1 && unit <= 5)
					bpm *= unitQuarters[unit];
				tempos.push_back({int(num(a->childText("Bar"))), std::clamp(num(a->childText("Position")), 0.0, 1.0), std::clamp(bpm, limits::kMinBpm, limits::kMaxBpm)});
			}
	std::sort(tempos.begin(), tempos.end(), [](auto &a, auto &b) { return a.bar < b.bar || (a.bar == b.bar && a.ratio < b.ratio); });
	score.tempo = 120;
	for (auto &t : tempos)
		if (t.bar == 0 && t.ratio == 0) {
			score.tempo = std::round(t.bpm);
			break;
		}
	if (score.tempo == 120 && !tempos.empty() && !(tempos[0].bar == 0 && tempos[0].ratio == 0))
		score.tempo = std::round(tempos[0].bpm);

	std::vector<int> staffCounts;
	std::vector<std::unordered_map<std::string, std::string>> chordNames;
	if (auto tl = gpif->child("Tracks"))
		for (auto &t : tl->children) {
			if (t->name != "Track")
				continue;
			if (score.tracks.size() >= size_t(limits::kMaxTracks))
				break;
			Track track;
			track.name = t->childText("Name");
			const XNode *firstStaff = nullptr;
			int staves = 0;
			if (auto st = t->child("Staves"))
				for (auto &s : st->children)
					if (s->name == "Staff") {
						if (!firstStaff) firstStaff = s.get();
						staves++;
					}
			staffCounts.push_back(std::max(1, staves));
			Props p = props(t.get());
			for (auto &kv : props(firstStaff))
				p[kv.first] = kv.second;
			auto pitches = ids(propText(p, "Tuning", "Pitches"));
			if (pitches.size() > size_t(limits::kMaxStrings))
				pitches.resize(size_t(limits::kMaxStrings));
			for (auto &pc : pitches)
				pc = std::clamp(pc, 0, 127);
			track.tuning.assign(pitches.rbegin(), pitches.rend());
			track.capo = int(num(propText(p, "CapoFret", "Fret")));
			track.percussion = track.tuning.empty();
			std::unordered_map<std::string, std::string> chords;
			for (const char *coll : {"DiagramCollection", "ChordCollection"}) {
				auto it = p.find(coll);
				if (it == p.end()) continue;
				if (auto items = it->second->child("Items"))
					for (auto &item : items->children)
						chords[item->attr("id")] = item->attr("name");
			}
			chordNames.push_back(std::move(chords));
			if (auto is = t->child("InstrumentSet")) {
				auto type = toLower(is->childText("Type"));
				if (type.find("drum") != std::string::npos || type.find("percussion") != std::string::npos)
					track.percussion = true;
			}
			score.tracks.push_back(std::move(track));
		}

	static const std::unordered_map<std::string, int> kValues = {{"Whole", 1}, {"Half", 2}, {"Quarter", 4}, {"Eighth", 8}, {"16th", 16}, {"32nd", 32}, {"64th", 64}, {"128th", 128}, {"DoubleWhole", 1}};
	static const std::unordered_map<std::string, int> kDynamics = {{"PPP", 1}, {"PP", 2}, {"P", 3}, {"MP", 4}, {"MF", 5}, {"F", 6}, {"FF", 7}, {"FFF", 8}};

	auto readBeat = [&](const XNode *node, size_t ti, int &beamNext) {
		Beat beat;
		beamNext = 0;
		if (auto xp = node->child("XProperties"))
			for (auto &x : xp->children) {
				if (x->name != "XProperty")
					continue;
				auto id = x->attr("id");
				int v = int(num(x->childText("Int")));
				if (id == "1124204546" && (v == 1 || v == 2))
					beamNext = v == 1 ? 2 : 1; // 1 = merge with the next beat, 2 = split
				else if (id == "1124204552" && v == 1 && !beamNext)
					beamNext = 3;
			}
		auto &track = score.tracks[ti];
		if (auto rh = node->child("Rhythm")) {
			auto it = rhythmsById.find(rh->attr("ref"));
			if (it != rhythmsById.end()) {
				auto v = kValues.find(it->second->childText("NoteValue"));
				beat.value = v == kValues.end() ? 4 : v->second;
				if (auto dot = it->second->child("AugmentationDot"))
					beat.dots = std::clamp(int(num(dot->attr("count"))), 0, 2);
				if (auto tup = it->second->child("PrimaryTuplet")) {
					beat.tupletNum = std::clamp(int(num(tup->attr("num"))), 0, 32);
					beat.tupletDen = std::clamp(int(num(tup->attr("den"))), 0, 32);
				}
			}
		}
		auto dyn = kDynamics.find(node->childText("Dynamic"));
		auto chordId = node->childText("Chord");
		if (!chordId.empty()) {
			auto it = chordNames[ti].find(chordId);
			if (it != chordNames[ti].end())
				beat.chord = it->second;
		}
		beat.text = node->childText("FreeText");
		auto trem = node->childText("Tremolo");
		if (auto slash = trem.find('/'); slash != std::string::npos)
			beat.tremolo = std::clamp(int(num(trem.substr(slash + 1), 16)), 8, 32);
		beat.fadeIn = node->childText("Fadding") == "FadeIn";
		Props bp = props(node);
		auto pick = propText(bp, "PickStroke", "Direction");
		if (!pick.empty())
			beat.pickStroke = pick == "Up" ? Stroke::Up : Stroke::Down;
		auto brush = propText(bp, "Brush", "Direction");
		if (!brush.empty())
			beat.brush = brush == "Up" ? Stroke::Up : Stroke::Down;
		if (auto grace = node->childText("GraceNotes"); !grace.empty())
			beat.grace = grace == "OnBeat" ? 2 : 1;
		beat.technique = bp.count("Slapped") ? 2 : bp.count("Popped") ? 3 : 0;
		if (bp.count("WhammyBar")) {
			auto f = [&](const char *name, double fallback) { return bp.count(name) ? num(propText(bp, name, "Float")) : fallback; };
			auto semis = [](double v) { return float(std::clamp(v / 50, -double(limits::kMaxBendSemis), double(limits::kMaxBendSemis))); };
			beat.whammy.push_back({float(f("WhammyBarOriginOffset", 0) / 100), semis(f("WhammyBarOriginValue", 0))});
			if (bp.count("WhammyBarMiddleValue")) {
				float mv = semis(f("WhammyBarMiddleValue", 0));
				beat.whammy.push_back({float(f("WhammyBarMiddleOffset1", 50) / 100), mv});
				if (bp.count("WhammyBarMiddleOffset2"))
					beat.whammy.push_back({float(f("WhammyBarMiddleOffset2", 50) / 100), mv});
			}
			beat.whammy.push_back({float(f("WhammyBarDestinationOffset", 100) / 100), semis(f("WhammyBarDestinationValue", 0))});
			for (auto &p : beat.whammy)
				p.t = std::clamp(p.t, 0.f, 1.f);
		}
		int strings = int(track.tuning.size());
		for (int nid : ids(node->childText("Notes"))) {
			if (beat.notes.size() >= size_t(limits::kMaxStrings))
				break;
			auto it = notesById.find(std::to_string(nid));
			if (it == notesById.end() || !strings)
				continue;
			const XNode *nn = it->second;
			Props p = props(nn);
			auto sText = propText(p, "String", "String");
			if (sText.empty())
				continue;
			Note note;
			int sIndex = int(num(sText, -1));
			if (sIndex < 0 || sIndex >= strings)
				continue;
			note.string = strings - 1 - sIndex;
			note.fret = std::clamp(int(num(propText(p, "Fret", "Fret"))), 0, limits::kMaxFret);
			if (dyn != kDynamics.end())
				note.dynamic = dyn->second;
			if (auto tie = nn->child("Tie"))
				note.tie = tie->attr("destination") == "true";
			note.letRing = nn->child("LetRing");
			note.vibrato = nn->child("Vibrato");
			note.wideVibrato = nn->childText("Vibrato") == "Wide";
			if (auto trill = nn->childText("Trill"); !trill.empty())
				note.trill = std::clamp(int(num(trill)), 0, limits::kMaxFret);
			if (p.count("Tapped"))
				beat.technique = 1;
			note.ghost = nn->child("AntiAccent");
			int accent = int(num(nn->childText("Accent")));
			note.staccato = accent & 1;
			note.accent = (accent & 4) ? 2 : (accent & 8) ? 1 : 0;
			note.palmMute = p.count("PalmMuted");
			note.dead = p.count("Muted");
			note.hammer = p.count("HopoOrigin");
			int slide = int(num(propText(p, "Slide", "Flags")));
			note.slide = (slide & 1) ? Slide::Shift : (slide & 2) ? Slide::Legato : (slide & 4) ? Slide::OutDown : (slide & 8) ? Slide::OutUp : Slide::None;
			note.slideIn = (slide & 16) ? SlideIn::Below : (slide & 32) ? SlideIn::Above : SlideIn::None;
			auto ht = propText(p, "HarmonicType", "HType");
			if (!ht.empty() && ht != "NoHarmonic") {
				note.harmonic = ht == "Natural" ? Harmonic::Natural : ht == "Tap" ? Harmonic::Tap : ht == "Pinch" ? Harmonic::Pinch : ht == "Semi" ? Harmonic::Semi : Harmonic::Artificial;
				note.harmonicFret = float(std::clamp(num(propText(p, "HarmonicFret", "HFret")), 0.0, double(limits::kMaxFret)));
			}
			if (p.count("Bended")) {
				auto f = [&](const char *name) { return num(propText(p, name, "Float")); };
				note.bend.push_back({0, float(f("BendOriginValue") / 50)});
				if (p.count("BendMiddleValue")) {
					float mv = float(f("BendMiddleValue") / 50);
					note.bend.push_back({float((p.count("BendMiddleOffset1") ? f("BendMiddleOffset1") : 50) / 100), mv});
					if (p.count("BendMiddleOffset2"))
						note.bend.push_back({float(f("BendMiddleOffset2") / 100), mv});
				}
				note.bend.push_back({float(p.count("BendDestinationOffset") ? f("BendDestinationOffset") / 100 : 1), float(f("BendDestinationValue") / 50)});
			}
			beat.notes.push_back(std::move(note));
		}
		beat.rest = beat.notes.empty();
		return beat;
	};

	if (auto mbs = gpif->child("MasterBars")) {
		int index = 0;
		MasterBar prev;
		for (auto &node : mbs->children) {
			if (node->name != "MasterBar")
				continue;
			if (index >= limits::kMaxBars)
				throw std::runtime_error("Too many bars");
			MasterBar mb;
			auto time = node->childText("Time");
			auto slash = time.find('/');
			mb.num = slash == std::string::npos ? prev.num : std::clamp(int(num(time.substr(0, slash), 4)), 1, 64);
			mb.den = slash == std::string::npos ? prev.den : std::clamp(int(num(time.substr(slash + 1), 4)), 1, 64);
			if (auto rep = node->child("Repeat")) {
				mb.repeatStart = rep->attr("start") == "true";
				if (rep->attr("end") == "true")
					mb.repeatPlays = std::clamp(int(num(rep->attr("count"), 2)), 2, 64);
			}
			for (int e : ids(node->childText("AlternateEndings")))
				if (e >= 1 && e <= 32)
					mb.alternate |= 1u << (e - 1);
			if (auto sec = node->child("Section"))
				mb.marker = sec->childText("Text");
			mb.doubleBar = node->child("DoubleBar");
			if (auto dirs = node->child("Directions"))
				for (auto &d : dirs->children) {
					static const char *kTargets[5] = {"Coda", "DoubleCoda", "Segno", "SegnoSegno", "Fine"};
					static const char *kJumps[14] = {"DaCapo", "DaCapoAlCoda", "DaCapoAlDoubleCoda", "DaCapoAlFine", "DaSegno", "DaSegnoAlCoda", "DaSegnoAlDoubleCoda",
									 "DaSegnoAlFine", "DaSegnoSegno", "DaSegnoSegnoAlCoda", "DaSegnoSegnoAlDoubleCoda", "DaSegnoSegnoAlFine", "DaCoda", "DaDoubleCoda"};
					auto t = trim(d->text);
					if (d->name == "Target")
						for (int k = 0; k < 5; k++)
							if (t == kTargets[k])
								mb.targets |= uint8_t(1 << k);
					if (d->name == "Jump")
						for (int k = 0; k < 14; k++)
							if (t == kJumps[k])
								mb.jump = Jump(k + 1);
				}
			if (auto fs = node->child("Fermatas"))
				for (auto &f : fs->children) {
					if (f->name != "Fermata" || mb.fermatas.size() >= 16)
						continue;
					// the offset is in quarter notes, as a fraction ("3/2")
					auto off = f->childText("Offset");
					auto cut = off.find('/');
					double quarters = cut == std::string::npos ? 0 : num(off.substr(0, cut)) / std::max(1.0, num(off.substr(cut + 1), 1));
					Fermata fm;
					fm.ratio = std::clamp(quarters * kQuarter / barTicks(mb), 0.0, 1.0);
					auto len = f->childText("Length");
					fm.length = std::clamp(len.empty() ? 0.5 : num(len), 0.0, 4.0);
					mb.fermatas.push_back(fm);
				}
			static const std::unordered_map<std::string, Feel> kFeels = {{"Triplet8th", Feel::Triplet8th}, {"Triplet16th", Feel::Triplet16th}, {"Dotted8th", Feel::Dotted8th}, {"Dotted16th", Feel::Dotted16th}, {"Scottish8th", Feel::Scottish8th}, {"Scottish16th", Feel::Scottish16th}};
			auto feel = kFeels.find(node->childText("TripletFeel"));
			mb.feel = feel == kFeels.end() ? Feel::None : feel->second;
			for (auto &t : tempos)
				if (t.bar == index && (index > 0 || t.ratio > 0))
					mb.tempoChanges.push_back({t.ratio, t.bpm});
			auto barIds = ids(node->childText("Bars"));
			size_t k = 0;
			for (size_t ti = 0; ti < score.tracks.size(); ti++) {
				Bar bar;
				if (k < barIds.size()) {
					auto bit = barsById.find(std::to_string(barIds[k]));
					if (bit != barsById.end())
						for (int vid : ids(bit->second->childText("Voices"))) {
							if (vid < 0) continue;
							auto vit = voicesById.find(std::to_string(vid));
							if (vit == voicesById.end()) continue;
							Voice voice;
							int beamNext = 0;
							for (int bid : ids(vit->second->childText("Beats"))) {
								if (voice.size() >= size_t(limits::kMaxBeatsPerVoice))
									throw std::runtime_error("Too many beats in a bar");
								auto beatIt = beatsById.find(std::to_string(bid));
								if (beatIt != beatsById.end()) {
									int prevNext = beamNext;
									voice.push_back(readBeat(beatIt->second, ti, beamNext));
									voice.back().beam = uint8_t(voice.size() > 1 ? prevNext : 0);
								}
							}
							bar.voices.push_back(std::move(voice));
						}
				}
				k += size_t(staffCounts[ti]);
				score.tracks[ti].bars.push_back(std::move(bar));
			}
			score.masterBars.push_back(mb);
			prev = mb;
			index++;
		}
	}
	finishScore(score);
	return score;
}

} // namespace tab
