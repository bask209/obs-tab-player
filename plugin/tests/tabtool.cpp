// SPDX-License-Identifier: Apache-2.0
// Developer tool for the native engine (no OBS needed):
//   tabtool dump <file>                 score as JSON
//   tabtool timeline <file>             playback timeline as JSON
//   tabtool layout <file> [width] [horizontal] [scale]  drawing primitives as JSON
//   tabtool render <file> <out.png> [width] [theme]   draw the tab with the native renderer
//   tabtool wav <file> <out.wav> [seconds]            render audio with the native synth
//   tabtool bench <folder>              parse + timeline + layout every file, print timings
#include "core/model.hpp"
#include "core/timeline.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
static size_t privateBytes()
{
	PROCESS_MEMORY_COUNTERS_EX pmc{};
	GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof pmc);
	return pmc.PrivateUsage;
}
#endif

#include "core/json.hpp"
#include "core/layout.hpp"

#ifdef TAB_WITH_RENDERER
#include "core/synth.hpp"
#include "win/d2d_renderer.hpp"
#endif

using namespace tab;
namespace fs = std::filesystem;

static std::vector<uint8_t> readFile(const fs::path &p)
{
	std::ifstream in(p, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		std::cerr << "usage: tabtool dump|timeline|render|wav|bench <file> ...\n";
		return 2;
	}
	std::string cmd = argv[1];
	try {
#ifdef TAB_WITH_RENDERER
		if (cmd == "soak") {
			// Repeats load + timeline + layout + tile render + synth for every file, many times.
			// Private memory must not grow between rounds (no leaks).
			std::vector<std::vector<uint8_t>> files;
			for (auto &e : fs::recursive_directory_iterator(fs::u8path(argv[2])))
				if (e.is_regular_file() && e.path().extension() == ".gp5")
					files.push_back(readFile(e.path()));
			int rounds = argc > 3 ? std::stoi(argv[3]) : 5;
			Renderer r;
			Synth synth(48000);
			std::vector<float> buf(4800);
			size_t first = 0;
			for (int round = 0; round < rounds; round++) {
				for (auto &d : files) {
					auto s = loadScore(d.data(), d.size());
					auto tl = buildTimeline(s, 0);
					LayoutOptions lo;
					lo.width = 1200;
					lo.scale = 1.4f;
					auto lay = layoutTrack(s, 0, lo);
					auto img = r.renderSheet(lay, themeByName("dark"), 0, 0, std::min(1024, int(lay.width)), std::min(1024, int(lay.height)));
					auto ci = countInClicks(s, tl, 0);
					synth.load(tl, &ci);
					synth.play(0, 1.0, true, true);
					synth.render(buf.data(), buf.size());
				}
				size_t now = privateBytes();
				if (round == 1)
					first = now;
				std::printf("round %d: private memory %.1f MB\n", round + 1, now / 1048576.0);
			}
			size_t last = privateBytes();
			std::printf("growth after warm-up: %.2f MB over %d rounds of %zu files\n", (double(last) - double(first)) / 1048576.0, rounds - 2, files.size());
			return 0;
		}
#endif
		if (cmd == "bench") {
			int files = 0;
			double parse = 0, timeline = 0, layout = 0;
			for (auto &e : fs::recursive_directory_iterator(fs::u8path(argv[2]))) {
				auto ext = e.path().extension().string();
				if (ext != ".gp5" && ext != ".gp4" && ext != ".gp3" && ext != ".gpx" && ext != ".gp")
					continue;
				auto data = readFile(e.path());
				auto t0 = std::chrono::steady_clock::now();
				auto s = loadScore(data.data(), data.size());
				auto t1 = std::chrono::steady_clock::now();
				auto tl = buildTimeline(s, 0);
				auto t2 = std::chrono::steady_clock::now();
#ifdef TAB_WITH_RENDERER
				LayoutOptions lo;
				auto lay = layoutTrack(s, 0, lo);
#endif
				auto t3 = std::chrono::steady_clock::now();
				parse += std::chrono::duration<double, std::milli>(t1 - t0).count();
				timeline += std::chrono::duration<double, std::milli>(t2 - t1).count();
				layout += std::chrono::duration<double, std::milli>(t3 - t2).count();
				files++;
			}
			std::printf("%d files: parse %.3f ms, timeline %.3f ms, layout %.3f ms (average per file)\n", files, parse / files, timeline / files, layout / files);
			return 0;
		}
		auto data = readFile(fs::u8path(argv[2]));
		auto score = loadScore(data.data(), data.size());
		if (cmd == "dump") {
			std::cout << scoreJson(score);
		} else if (cmd == "timeline") {
			std::cout << timelineJson(buildTimeline(score, 0));
		} else if (cmd == "layout") {
			// drawing primitives and cursor geometry: layout <file> [width] [horizontal] [scale]
			LayoutOptions lo;
			lo.width = argc > 3 ? std::stof(argv[3]) : 900.f;
			lo.horizontal = argc > 4 && std::string(argv[4]) == "1";
			lo.scale = argc > 5 ? std::stof(argv[5]) : 1.f;
			std::cout << layoutJson(layoutTrack(score, 0, lo));
		}
#ifdef TAB_WITH_RENDERER
		else if (cmd == "render") {
			LayoutOptions lo;
			lo.width = argc > 4 ? std::stof(argv[4]) : 1280.f;
			lo.scale = lo.width / 900.f;
			Theme theme = themeByName(argc > 5 ? argv[5] : "dark");
			auto lay = layoutTrack(score, 0, lo);
			auto t0 = std::chrono::steady_clock::now();
			auto img = renderLayout(lay, theme, true);
			auto t1 = std::chrono::steady_clock::now();
			savePng(img, fs::u8path(argv[3]));
			std::printf("%dx%d, %zu primitives, rendered in %.1f ms\n", img.width, img.height, lay.prims.size(), std::chrono::duration<double, std::milli>(t1 - t0).count());
		} else if (cmd == "wav") {
			auto tl = buildTimeline(score, 0);
			double seconds = argc > 4 ? std::stod(argv[4]) : tl.duration;
			Synth synth(48000);
			synth.load(tl);
			synth.play(0, 1.0, false, true);
			std::vector<float> pcm(size_t(seconds * 48000));
			auto t0 = std::chrono::steady_clock::now();
			for (size_t i = 0; i < pcm.size(); i += 480)
				synth.render(pcm.data() + i, std::min<size_t>(480, pcm.size() - i));
			auto t1 = std::chrono::steady_clock::now();
			saveWav(pcm, 48000, fs::u8path(argv[3]));
			std::printf("%.1f s of audio rendered in %.1f ms\n", seconds, std::chrono::duration<double, std::milli>(t1 - t0).count());
		}
#endif
		else {
			std::cerr << "unknown command\n";
			return 2;
		}
	} catch (const std::exception &e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
