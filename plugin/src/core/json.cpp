// SPDX-License-Identifier: Apache-2.0
#include "json.hpp"

#include <cmath>
#include <cstdio>

namespace tab {
namespace {

class Json {
public:
	std::string s;

	Json &raw(const char *t) { s += t; return *this; }
	Json &str(const std::string &v)
	{
		s += '"';
		for (unsigned char c : v) {
			if (c == '"' || c == '\\') { s += '\\'; s += char(c); }
			else if (c == '\n') s += "\\n";
			else if (c == '\r') s += "\\r";
			else if (c == '\t') s += "\\t";
			else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); s += b; }
			else s += char(c);
		}
		s += '"';
		return *this;
	}
	Json &num(double v)
	{
		if (!std::isfinite(v))
			v = 0;
		char b[32];
		std::snprintf(b, sizeof b, "%.17g", v);
		s += b;
		return *this;
	}
	Json &fixed(double v) // drawing coordinates: two decimals are plenty
	{
		if (!std::isfinite(v))
			v = 0;
		char b[32];
		std::snprintf(b, sizeof b, "%.2f", v);
		s += b;
		return *this;
	}
	Json &boolean(bool v) { s += v ? "true" : "false"; return *this; }
	// ,"key": when the value is present
	Json &key(const char *k) { if (s.back() != '{') s += ','; s += '"'; s += k; s += "\":"; return *this; }
	Json &flag(const char *k, bool v) { if (v) key(k).boolean(true); return *this; }
	Json &text(const char *k, const std::string &v) { if (!v.empty()) key(k).str(v); return *this; }
	Json &points(const std::vector<BendPoint> &pts)
	{
		s += '[';
		for (size_t i = 0; i < pts.size(); i++) {
			s += i ? ",{\"t\":" : "{\"t\":";
			num(pts[i].t);
			s += ",\"semis\":";
			num(pts[i].semis);
			s += '}';
		}
		s += ']';
		return *this;
	}
};

const char *strokeName(Stroke s) { return s == Stroke::Up ? "up" : s == Stroke::Down ? "down" : ""; }

const char *slideName(Slide s)
{
	switch (s) {
	case Slide::Shift: return "shift";
	case Slide::Legato: return "legato";
	case Slide::OutDown: return "outDown";
	case Slide::OutUp: return "outUp";
	default: return "";
	}
}

const char *harmonicName(Harmonic h)
{
	switch (h) {
	case Harmonic::Natural: return "natural";
	case Harmonic::Artificial: return "artificial";
	case Harmonic::Tap: return "tap";
	case Harmonic::Pinch: return "pinch";
	case Harmonic::Semi: return "semi";
	default: return "";
	}
}

const char *feelName(Feel f)
{
	switch (f) {
	case Feel::Triplet8th: return "Triplet8th";
	case Feel::Triplet16th: return "Triplet16th";
	case Feel::Dotted8th: return "Dotted8th";
	case Feel::Dotted16th: return "Dotted16th";
	case Feel::Scottish8th: return "Scottish8th";
	case Feel::Scottish16th: return "Scottish16th";
	default: return "";
	}
}

const char *kindName(NoteKind k)
{
	switch (k) {
	case NoteKind::Dead: return "dead";
	case NoteKind::Muted: return "muted";
	case NoteKind::Harmonic: return "harmonic";
	default: return "pluck";
	}
}

void note(Json &j, const Note &n)
{
	j.raw("{\"string\":").num(n.string).raw(",\"fret\":").num(n.fret);
	j.flag("tie", n.tie).flag("dead", n.dead).flag("ghost", n.ghost).flag("hammer", n.hammer);
	j.flag("letRing", n.letRing).flag("palmMute", n.palmMute).flag("staccato", n.staccato);
	j.flag("vibrato", n.vibrato).flag("wideVibrato", n.wideVibrato);
	if (n.accent == 2)
		j.key("accent").str("heavy");
	else if (n.accent)
		j.key("accent").boolean(true);
	if (n.dynamic >= 1)
		j.key("dynamic").num(n.dynamic);
	j.text("slide", slideName(n.slide));
	if (n.slideIn != SlideIn::None)
		j.key("slideIn").str(n.slideIn == SlideIn::Below ? "below" : "above");
	j.text("harmonic", harmonicName(n.harmonic));
	if (n.harmonicFret > 0)
		j.key("harmonicFret").num(n.harmonicFret);
	if (n.trill >= 0)
		j.key("trill").num(n.trill).key("trillSpeed").num(n.trillSpeed);
	if (!n.bend.empty())
		j.key("bend").points(n.bend);
	if (n.grace) {
		j.key("grace").raw("{\"fret\":").num(n.grace->fret);
		j.flag("slide", n.grace->slide).flag("hammer", n.grace->hammer).flag("dead", n.grace->dead).flag("onBeat", n.grace->onBeat);
		j.raw("}");
	}
	j.raw("}");
}

void beat(Json &j, const Beat &b)
{
	j.raw("{\"value\":").num(b.value).raw(",\"dots\":").num(b.dots).raw(",\"tuplet\":");
	if (b.tupletNum)
		j.raw("[").num(b.tupletNum).raw(",").num(b.tupletDen).raw("]");
	else
		j.raw("null");
	j.raw(",\"ticks\":").num(b.ticks).raw(",\"start\":").num(b.start).raw(",\"rest\":").boolean(b.rest).raw(",\"notes\":[");
	for (size_t i = 0; i < b.notes.size(); i++) {
		if (i)
			j.raw(",");
		note(j, b.notes[i]);
	}
	j.raw("]");
	j.flag("fullBar", b.fullBar).text("chord", b.chord).text("text", b.text);
	j.text("pickStroke", strokeName(b.pickStroke)).text("brush", strokeName(b.brush));
	j.flag("vibrato", b.vibrato).flag("fadeIn", b.fadeIn).flag("letRing", b.letRing);
	if (b.tremolo)
		j.key("tremolo").num(b.tremolo);
	j.flag("tap", b.technique == 1).flag("slap", b.technique == 2).flag("pop", b.technique == 3);
	if (b.tempo > 0)
		j.key("tempo").num(b.tempo);
	if (b.beam)
		j.key("beam").num(b.beam);
	if (!b.whammy.empty())
		j.key("whammy").points(b.whammy);
	j.raw("}");
}

} // namespace

std::string scoreJson(const Score &s)
{
	Json j;
	j.raw("{\"format\":").str(s.format);
	j.raw(",\"title\":").str(s.title).raw(",\"subTitle\":").str(s.subTitle).raw(",\"artist\":").str(s.artist);
	j.raw(",\"album\":").str(s.album).raw(",\"words\":").str(s.words).raw(",\"music\":").str(s.music);
	j.raw(",\"copyright\":").str(s.copyright).raw(",\"tab\":").str(s.tab).raw(",\"instructions\":").str(s.instructions);
	j.raw(",\"notices\":").str(s.notices).raw(",\"tempoLabel\":").str(s.tempoLabel).raw(",\"tempo\":").num(s.tempo);
	j.raw(",\"masterBars\":[");
	for (size_t i = 0; i < s.masterBars.size(); i++) {
		auto &m = s.masterBars[i];
		j.raw(i ? ",{" : "{").raw("\"num\":").num(m.num).raw(",\"den\":").num(m.den);
		j.raw(",\"repeatStart\":").boolean(m.repeatStart).raw(",\"repeatPlays\":").num(m.repeatPlays);
		j.raw(",\"alternate\":").num(m.alternate).raw(",\"marker\":").str(m.marker).raw(",\"doubleBar\":").boolean(m.doubleBar);
		j.text("feel", feelName(m.feel));
		if (!m.tempoChanges.empty()) {
			j.key("tempoChanges").raw("[");
			for (size_t k = 0; k < m.tempoChanges.size(); k++)
				j.raw(k ? ",{\"ratio\":" : "{\"ratio\":").num(m.tempoChanges[k].ratio).raw(",\"bpm\":").num(m.tempoChanges[k].bpm).raw("}");
			j.raw("]");
		}
		if (m.targets)
			j.key("targets").num(m.targets);
		if (m.jump != Jump::None)
			j.key("jump").num(int(m.jump));
		if (!m.fermatas.empty()) {
			j.key("fermatas").raw("[");
			for (size_t k = 0; k < m.fermatas.size(); k++)
				j.raw(k ? ",{\"ratio\":" : "{\"ratio\":").num(m.fermatas[k].ratio).raw(",\"length\":").num(m.fermatas[k].length).raw("}");
			j.raw("]");
		}
		j.raw("}");
	}
	j.raw("],\"tracks\":[");
	for (size_t t = 0; t < s.tracks.size(); t++) {
		auto &tr = s.tracks[t];
		j.raw(t ? ",{" : "{").raw("\"name\":").str(tr.name).raw(",\"capo\":").num(tr.capo).raw(",\"percussion\":").boolean(tr.percussion).raw(",\"tuning\":[");
		for (size_t k = 0; k < tr.tuning.size(); k++)
			j.raw(k ? "," : "").num(tr.tuning[k]);
		j.raw("],\"bars\":[");
		for (size_t b = 0; b < tr.bars.size(); b++) {
			j.raw(b ? ",{\"voices\":[" : "{\"voices\":[");
			auto &voices = tr.bars[b].voices;
			for (size_t v = 0; v < voices.size(); v++) {
				j.raw(v ? ",[" : "[");
				for (size_t k = 0; k < voices[v].size(); k++) {
					if (k)
						j.raw(",");
					beat(j, voices[v][k]);
				}
				j.raw("]");
			}
			j.raw("]}");
		}
		j.raw("]}");
	}
	j.raw("]}");
	return std::move(j.s);
}

std::string timelineJson(const Timeline &tl)
{
	Json j;
	j.raw("{\"duration\":").num(tl.duration).raw(",\"order\":[");
	for (size_t i = 0; i < tl.order.size(); i++)
		j.raw(i ? "," : "").num(tl.order[i]);
	j.raw("],\"bars\":[");
	for (size_t i = 0; i < tl.bars.size(); i++) {
		auto &b = tl.bars[i];
		j.raw(i ? ",{\"index\":" : "{\"index\":").num(b.index).raw(",\"start\":").num(b.start).raw(",\"end\":").num(b.end).raw(",\"bpm\":").num(b.bpm).raw("}");
	}
	j.raw("],\"beats\":[");
	for (size_t i = 0; i < tl.beats.size(); i++) {
		auto &b = tl.beats[i];
		j.raw(i ? ",{\"t\":" : "{\"t\":").num(b.t).raw(",\"dur\":").num(b.dur).raw(",\"bar\":").num(b.bar).raw(",\"beat\":").num(b.beat).raw(",\"occ\":").num(b.occ).raw("}");
	}
	j.raw("],\"clicks\":[");
	for (size_t i = 0; i < tl.clicks.size(); i++)
		j.raw(i ? ",{\"t\":" : "{\"t\":").num(tl.clicks[i].t).raw(",\"down\":").boolean(tl.clicks[i].down).raw("}");
	j.raw("],\"notes\":[");
	for (size_t i = 0; i < tl.notes.size(); i++) {
		auto &n = tl.notes[i];
		j.raw(i ? ",{\"t\":" : "{\"t\":").num(n.t).raw(",\"dur\":").num(n.dur).raw(",\"midi\":").num(n.midi).raw(",\"vel\":").num(n.vel);
		j.raw(",\"track\":").num(n.track).raw(",\"string\":").num(n.string).raw(",\"kind\":").str(kindName(n.kind));
		j.flag("legato", n.legato).flag("ring", n.ring).flag("vibrato", n.vibrato).flag("hammer", n.hammer);
		j.flag("slideNext", n.slideNext).flag("slideLegato", n.slideLegato);
		if (n.slideTarget >= 0)
			j.key("slideTarget").num(n.slideTarget);
		if (n.slideOut != 0)
			j.key("slideOut").num(n.slideOut);
		if (n.slideIn != 0)
			j.key("slideIn").num(n.slideIn);
		if (!n.bend.empty())
			j.key("bend").points(n.bend);
		j.raw("}");
	}
	j.raw("]}");
	return std::move(j.s);
}

std::string layoutJson(const Layout &lay)
{
	Json j;
	j.raw("{\"width\":").num(lay.width).raw(",\"height\":").num(lay.height).raw(",\"prims\":[");
	// [type, role, x0, y0, x1, y1, size, align, bold, italic, string, beatKey, text, path, stroke]
	for (size_t i = 0; i < lay.prims.size(); i++) {
		auto &p = lay.prims[i];
		j.raw(i ? ",[" : "[").num(int(p.type)).raw(",").num(int(p.role)).raw(",");
		j.fixed(p.x0).raw(",").fixed(p.y0).raw(",").fixed(p.x1).raw(",").fixed(p.y1).raw(",").fixed(p.size).raw(",");
		j.num(p.align).raw(",").num(p.bold ? 1 : 0).raw(",").num(p.italic ? 1 : 0).raw(",").num(p.string).raw(",").num(p.beatKey).raw(",").str(p.text).raw(",\"");
		for (auto &op : p.path) {
			j.s += op.op;
			int n = op.op == 'Z' ? 0 : op.op == 'Q' ? 2 : op.op == 'C' ? 3 : 1;
			for (int k = 0; k < n; k++)
				j.fixed(op.x[k]).raw(" ").fixed(op.y[k]).raw(k + 1 < n ? " " : "");
		}
		j.raw("\",").fixed(p.stroke).raw("]");
	}
	j.raw("],\"bars\":[");
	for (size_t i = 0; i < lay.bars.size(); i++) {
		auto &b = lay.bars[i];
		j.raw(i ? ",{\"index\":" : "{\"index\":").num(b.index).raw(",\"sys\":").num(b.sys).raw(",\"x0\":").fixed(b.x0).raw(",\"x1\":").fixed(b.x1).raw("}");
	}
	j.raw("],\"systems\":[");
	for (size_t i = 0; i < lay.systems.size(); i++) {
		auto &s = lay.systems[i];
		j.raw(i ? ",{\"y\":" : "{\"y\":").fixed(s.y).raw(",\"top\":").fixed(s.top).raw(",\"bottom\":").fixed(s.bottom).raw(",\"height\":").fixed(s.height);
		j.raw(",\"x0\":").fixed(s.x0).raw(",\"x1\":").fixed(s.x1).raw("}");
	}
	j.raw("],\"beats\":[");
	bool first = true;
	for (auto &[k, x] : lay.beats) {
		j.raw(first ? "[" : ",[").num(k).raw(",").fixed(x).raw("]");
		first = false;
	}
	j.raw("]}");
	return std::move(j.s);
}

} // namespace tab
