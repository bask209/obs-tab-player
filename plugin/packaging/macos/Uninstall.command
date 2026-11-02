#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Removes OBS Tab Player. Your tab files are not touched.
set -euo pipefail
rm -rf "$HOME/Library/Application Support/obs-studio/plugins/obs-tab-player.plugin"
echo "Removed OBS Tab Player."
echo "Your settings and stars stay in ~/Library/Application Support/obs-studio/plugin_config/obs-tab-player."
read -r -p "Press Return to close." _
