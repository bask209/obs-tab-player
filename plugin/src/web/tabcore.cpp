// SPDX-License-Identifier: Apache-2.0
// The engine as a WebAssembly module, for the website and the tests: the same readers,
// timeline, layout and synth as the OBS plugin. Built by tools/build-web.mjs.
// One tab is open at a time. Results that are not numbers come back as JSON strings, valid
// until the next call; core.mjs next to this file wraps all of this for JavaScript.
#include "core/json.hpp"
#include "core/layout.hpp"
#include "core/model.hpp"
#include "core/synth.hpp"
#include "core/timeline.hpp"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace tab;

namespace {

struct Open {
	Score score;
	Timeline timeline;
	int track = 0;
};

std::vector<uint8_t> input;
std::unique_ptr<Open> open;
std::unique_ptr<Synth> synth;
std::vector<float> mainOut, clickOut;
std::string result;

const char *error(const std::string &message)
{
	result = "{\"error\":\"";
	for (char c : message)
		result += (c == '"' || c == '\\') ? ' ' : c;
	result += "\"}";
	return result.c_str();
}

void selectTrack(int track)
{
	open->track = std::clamp(track, 0, std::max(0, int(open->score.tracks.size()) - 1));
	open->timeline = buildTimeline(open->score, open->track);
	auto ci = countInClicks(open->score, open->timeline, 0);
	synth->load(open->timeline, &ci);
}

} // namespace

extern "C" {

// Room for a file of n bytes; the caller copies it there before tc_load.
EMSCRIPTEN_KEEPALIVE uint8_t *tc_input(int n)
{
	input.assign(size_t(std::max(0, n)), 0);
	return input.data();
}

// Reads the file in the input buffer. Returns {"track","tracks":[{"percussion"}]}
// or {"error"}. A band file opens on its first pitched track, as in OBS.
EMSCRIPTEN_KEEPALIVE const char *tc_load(int sampleRate)
{
	try {
		auto next = std::make_unique<Open>();
		next->score = loadScore(input.data(), input.size());
		if (next->score.tracks.empty())
			throw std::runtime_error("The file has no tracks");
		open = std::move(next);
		synth = std::make_unique<Synth>(std::clamp(sampleRate, 8000, 192000));
		int track = 0;
		if (open->score.tracks[0].percussion)
			for (size_t t = 1; t < open->score.tracks.size(); t++)
				if (!open->score.tracks[t].percussion) {
					track = int(t);
					break;
				}
		selectTrack(track);
		result = "{\"track\":" + std::to_string(open->track) + ",\"tracks\":[";
		for (size_t t = 0; t < open->score.tracks.size(); t++)
			result += std::string(t ? "," : "") + "{\"percussion\":" + (open->score.tracks[t].percussion ? "true" : "false") + "}";
		result += "]}";
		return result.c_str();
	} catch (const std::exception &e) {
		return error(e.what());
	}
}

EMSCRIPTEN_KEEPALIVE void tc_select_track(int track)
{
	if (open)
		selectTrack(track);
}

EMSCRIPTEN_KEEPALIVE const char *tc_score()
{
	result = open ? scoreJson(open->score) : "null";
	return result.c_str();
}

EMSCRIPTEN_KEEPALIVE const char *tc_timeline()
{
	result = open ? timelineJson(open->timeline) : "null";
	return result.c_str();
}

// labels: 0 TAB, 1 string names, 2 none
EMSCRIPTEN_KEEPALIVE const char *tc_layout(float width, int horizontal, float scale, int rhythm, int barNumbers, int labels)
{
	if (!open)
		return "null";
	LayoutOptions o;
	o.width = width;
	o.horizontal = horizontal != 0;
	o.scale = scale;
	o.rhythm = rhythm != 0;
	o.barNumbers = barNumbers != 0;
	o.labels = labels;
	result = layoutJson(layoutTrack(open->score, open->track, o));
	return result.c_str();
}

// ---- playback: the plugin's synth, rendered block by block into two buffers (part, clicks)

EMSCRIPTEN_KEEPALIVE void tc_play(double from, double speed, int countIn, int loop)
{
	if (synth)
		synth->play(from, speed, countIn != 0, loop != 0);
}
EMSCRIPTEN_KEEPALIVE void tc_pause() { if (synth) synth->pause(); }
EMSCRIPTEN_KEEPALIVE void tc_seek(double t) { if (synth) synth->seek(t); }
EMSCRIPTEN_KEEPALIVE void tc_speed(double s) { if (synth) synth->setSpeed(s); }
EMSCRIPTEN_KEEPALIVE void tc_loop(int on, double a, double b) { if (synth) synth->setLoop(on != 0, a, b); }
EMSCRIPTEN_KEEPALIVE void tc_metronome(int on) { if (synth) synth->setMetronome(on != 0); }
EMSCRIPTEN_KEEPALIVE int tc_playing() { return synth && synth->playing() ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE double tc_rendered() { return synth ? double(synth->samplesRendered()) : 0; }
EMSCRIPTEN_KEEPALIVE double tc_position(double sample) { return synth ? synth->positionAt(int64_t(sample)) : 0; }
EMSCRIPTEN_KEEPALIVE int tc_count_in(double sample) { return synth ? synth->countInBeatAt(int64_t(sample)) : 0; }

// Renders the next n samples; read them from tc_part() and tc_clicks().
EMSCRIPTEN_KEEPALIVE void tc_render(int n)
{
	mainOut.assign(size_t(std::max(0, n)), 0.f);
	clickOut.assign(size_t(std::max(0, n)), 0.f);
	if (synth && n > 0)
		synth->render(mainOut.data(), mainOut.size(), clickOut.data());
}
EMSCRIPTEN_KEEPALIVE float *tc_part() { return mainOut.data(); }
EMSCRIPTEN_KEEPALIVE float *tc_clicks() { return clickOut.data(); }

} // extern "C"
