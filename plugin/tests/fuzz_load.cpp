// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: any byte string must either load cleanly or be rejected with
// std::runtime_error -- never crash, hang, overflow, leak or exhaust memory.
// Build with -DTAB_FUZZ=ON (MSVC: /fsanitize=fuzzer,address), run:
//   tab-fuzz.exe corpus_dir -max_len=262144 -max_total_time=600 -rss_limit_mb=1024
#include "core/layout.hpp"
#include "core/library.hpp"
#include "core/model.hpp"
#include "core/synth.hpp"
#include "core/timeline.hpp"

#include <cstdint>
#include <stdexcept>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	try {
		tab::Score score = tab::loadScore(data, size);
		auto notes = tab::parseNotes(score.notices, score.tempoLabel);
		(void)notes;
		for (int t = 0; t < int(score.tracks.size()) && t < 2; t++) {
			auto tl = tab::buildTimeline(score, t);
			tab::LayoutOptions o;
			o.horizontal = (size & 1) != 0;
			auto lay = tab::layoutTrack(score, t, o);
			(void)lay;
			if (t == 0) {
				tab::Synth synth(8000); // low rate keeps the fuzzer fast
				auto ci = tab::countInClicks(score, tl, 0);
				synth.load(tl, &ci);
				synth.play(0, 1.0, true, true);
				std::vector<float> buf(800);
				for (int i = 0; i < 4; i++)
					synth.render(buf.data(), buf.size());
			}
		}
	} catch (const std::runtime_error &) {
		// rejected input: fine
	}
	return 0;
}
