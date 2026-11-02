#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Installs OBS Tab Player for your user (no root needed). Run it from this folder.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
config="${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio"
if [ -d "$HOME/.var/app/com.obsproject.Studio" ] && [ ! -d "$config" ]; then
	echo "Only the Flatpak version of OBS was found. This package is for OBS installed from"
	echo "the OBS PPA, your distribution or OBS's .deb; it does not load in the Flatpak."
	exit 1
fi
dest="$config/plugins/obs-tab-player"
if pgrep -x obs >/dev/null 2>&1; then
	echo "Please close OBS first, then run this again."
	exit 1
fi
rm -rf "$dest"
mkdir -p "$(dirname "$dest")"
cp -r "$here/obs-tab-player" "$dest"
echo "Installed to $dest"
echo "Start OBS: the Tab Player panel opens on the right. Demo tabs are in $here/demo-tabs"
