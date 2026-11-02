// SPDX-License-Identifier: Apache-2.0
// JSON views of the engine's results, for the website (WebAssembly), the tests and tabtool.
#pragma once

#include "layout.hpp"
#include "model.hpp"
#include "timeline.hpp"

#include <string>

namespace tab {

// The whole score. Fields left at their defaults are left out.
std::string scoreJson(const Score &score);
// The playback timeline: bars, cursor stops, clicks and every note event.
std::string timelineJson(const Timeline &tl);
// Drawing primitives plus the cursor geometry (bars, rows, beat positions).
std::string layoutJson(const Layout &layout);

} // namespace tab
