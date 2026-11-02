// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace tabobs {

inline constexpr const char *kTabSourceId = "tab_player_source";
// Audio-only source carrying the metronome and count-in clicks, so they get their own
// fader and track routing in the OBS mixer.
inline constexpr const char *kMetronomeSourceId = "tab_player_metronome";

void registerTabSource();
void showTabPlayerDock(); // implemented by the dock

} // namespace tabobs
