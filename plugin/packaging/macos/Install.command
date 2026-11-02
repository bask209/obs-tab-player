#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Installs OBS Tab Player for your user (no administrator password needed).
# Double-click it in Finder. The first time, macOS may ask: Control-click it and choose Open.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
dest="$HOME/Library/Application Support/obs-studio/plugins"
if pgrep -x OBS >/dev/null 2>&1; then
	echo "Please quit OBS first, then open Install.command again."
	read -r -p "Press Return to close." _
	exit 1
fi
mkdir -p "$dest"
rm -rf "$dest/obs-tab-player.plugin"
cp -R "$here/obs-tab-player.plugin" "$dest/"
# downloaded files are quarantined; OBS will not load a quarantined plugin
xattr -dr com.apple.quarantine "$dest/obs-tab-player.plugin" 2>/dev/null || true
echo "Installed to $dest/obs-tab-player.plugin"
echo "Start OBS: the Tab Player panel opens on the right. Demo tabs are in the demo-tabs folder."
read -r -p "Press Return to close." _
