// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace tabobs {

inline constexpr const char *kTabSourceId = "tab_player_source";

void registerTabSource();
void showTabPlayerDock(); // implemented by the dock

} // namespace tabobs
