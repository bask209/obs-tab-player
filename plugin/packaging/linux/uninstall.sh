#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Removes OBS Tab Player from your user's OBS plugins. Your tab files are not touched.
set -euo pipefail
dest="${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins/obs-tab-player"
rm -rf "$dest"
echo "Removed $dest"
echo "Your settings and stars stay in ${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugin_config/obs-tab-player (delete that folder to remove them too)."
