// SPDX-License-Identifier: Apache-2.0
#include "model.hpp"
#include "readers.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace tab {

int beatTicks(int value, int dots, int tupletNum, int tupletDen)
{
	double t = kQuarter * 4.0 / std::max(1, value);
	if (dots == 1)
		t *= 1.5;
	else if (dots >= 2)
		t *= 1.75;
	if (tupletNum > 1 && tupletDen > 0)
		t = t * tupletDen / tupletNum;
	return int(std::lround(t));
}

int barTicks(const MasterBar &mb)
{
	return int(std::lround(kQuarter * 4.0 * mb.num / std::max(1, mb.den)));
}

int naturalHarmonicOffset(int fret)
{
	switch (fret) {
	case 12: return 12;
	case 7: case 19: return 19;
	case 5: case 24: return 24;
	case 4: case 9: case 16: return 28;
	case 3: return 31;
	default: return fret + 12;
	}
}

void finishScore(Score &score)
{
	for (auto &track : score.tracks) {
		std::vector<std::vector<int>> last(2, std::vector<int>(16, -1));
		for (auto &bar : track.bars)
			for (size_t vi = 0; vi < bar.voices.size(); vi++) {
				if (vi >= last.size())
					last.resize(vi + 1, std::vector<int>(16, -1));
				for (auto &b : bar.voices[vi])
					for (auto &n : b.notes) {
						if (n.string < 0 || n.string >= 16)
							continue;
						if (n.tie && last[vi][n.string] >= 0)
							n.fret = last[vi][n.string];
						last[vi][n.string] = n.fret;
					}
			}
		for (size_t i = 0; i < track.bars.size(); i++) {
			auto &bar = track.bars[i];
			std::vector<Voice> kept;
			for (auto &v : bar.voices)
				if (!v.empty())
					kept.push_back(std::move(v));
			bar.voices = std::move(kept);
			for (auto &voice : bar.voices) {
				int pos = 0;
				for (auto &b : voice) {
					b.ticks = beatTicks(b.value, b.dots, b.tupletNum, b.tupletDen);
					b.start = pos;
					pos += b.ticks;
					std::sort(b.notes.begin(), b.notes.end(), [](const Note &a, const Note &c) { return a.string < c.string; });
				}
			}
			if (bar.voices.empty()) {
				Beat rest;
				rest.rest = true;
				rest.fullBar = true;
				rest.value = 1;
				rest.ticks = i < score.masterBars.size() ? barTicks(score.masterBars[i]) : kQuarter * 4;
				bar.voices.push_back({rest});
			}
		}
	}
}

Score loadScore(const uint8_t *d, size_t n)
{
	if (isGp3to5(d, n))
		return readGp3to5(d, n);
	if (isGpx(d, n)) {
		auto files = readGpxFiles(d, n);
		auto it = files.find("score.gpif");
		if (it == files.end())
			throw std::runtime_error("GPX file has no score");
		return readGpif(std::string(it->second.begin(), it->second.end()), "gpx");
	}
	if (isZip(d, n)) {
		std::vector<uint8_t> xml;
		if (!readZipEntry(d, n, "Content/score.gpif", xml))
			throw std::runtime_error("Not a Guitar Pro 7/8 file");
		return readGpif(std::string(xml.begin(), xml.end()), "gp7");
	}
	throw std::runtime_error("Unsupported or unknown file format");
}

} // namespace tab
