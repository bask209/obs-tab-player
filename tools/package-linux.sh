#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Linux packages from build/plugin/obs-tab-player.so:
#   dist/obs-tab-player-<version>-linux-x86_64.tar.gz  install.sh copies it into ~/.config/obs-studio/plugins
#   dist/obs-tab-player_<version>_amd64.deb            for OBS installed from the OBS PPA or the .deb
set -euo pipefail
cd "$(dirname "$0")/.."
version=$(node -p "require('./package.json').version")
so=build/plugin/obs-tab-player.so
[ -f "$so" ] || { echo "build the plugin first: $so is missing" >&2; exit 1; }
rm -rf dist/linux && mkdir -p dist/linux

# --- tar.gz for the user's own plugins folder (no root needed)
stage=dist/linux/obs-tab-player-$version
mkdir -p "$stage/obs-tab-player/bin/64bit" "$stage/demo-tabs"
cp "$so" "$stage/obs-tab-player/bin/64bit/"
cp -r plugin/data "$stage/obs-tab-player/data"
cp docs/demo/*.gp5 "$stage/demo-tabs/"
cp LICENSE "$stage/LICENSE.txt"
cp plugin/packaging/linux/README.txt plugin/packaging/linux/install.sh plugin/packaging/linux/uninstall.sh "$stage/"
chmod +x "$stage/install.sh" "$stage/uninstall.sh"
tar -C dist/linux -czf "dist/obs-tab-player-$version-linux-x86_64.tar.gz" "obs-tab-player-$version"

# --- .deb: where OBS's own package keeps its plugins
deb=dist/linux/deb
mkdir -p "$deb/DEBIAN" "$deb/usr/lib/x86_64-linux-gnu/obs-plugins" "$deb/usr/share/obs/obs-plugins/obs-tab-player" "$deb/usr/share/doc/obs-tab-player/demo-tabs"
install -m 0644 "$so" "$deb/usr/lib/x86_64-linux-gnu/obs-plugins/"
cp -r plugin/data/. "$deb/usr/share/obs/obs-plugins/obs-tab-player/"
cp docs/demo/*.gp5 "$deb/usr/share/doc/obs-tab-player/demo-tabs/"
cp LICENSE "$deb/usr/share/doc/obs-tab-player/copyright"
cat > "$deb/DEBIAN/control" <<EOF
Package: obs-tab-player
Version: $version
Section: video
Priority: optional
Architecture: amd64
Depends: obs-studio (>= 30)
Maintainer: OBS Tab Player <https://github.com/bask209/obs-tab-player/issues>
Homepage: https://bask209.github.io/obs-tab-player/
Description: Guitar Pro tabs in OBS Studio
 One scrolling line of tab with synced audio under your camera, with a control
 panel, tempo, loop, count-in and a metronome on its own mixer fader.
EOF
dpkg-deb --root-owner-group --build "$deb" "dist/obs-tab-player_${version}_amd64.deb" >/dev/null
ls -l dist/*linux* dist/*.deb
