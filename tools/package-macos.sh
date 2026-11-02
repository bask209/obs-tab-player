#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# macOS package from the built obs-tab-player.plugin (universal: Apple silicon and Intel):
#   dist/obs-tab-player-<version>-macos-universal.zip  Install.command copies it into
#   ~/Library/Application Support/obs-studio/plugins. The bundle is ad-hoc signed only; without
#   an Apple Developer ID, macOS quarantines downloads, which Install.command clears for the plugin.
set -euo pipefail
cd "$(dirname "$0")/.."
version=$(node -p "require('./package.json').version")
bundle=$(find build/plugin -maxdepth 3 -name 'obs-tab-player.plugin' -type d | head -1)
[ -n "$bundle" ] || { echo "build the plugin first: obs-tab-player.plugin is missing" >&2; exit 1; }
codesign --force --deep --sign - "$bundle"
rm -rf dist/macos && mkdir -p "dist/macos/OBS Tab Player $version/demo-tabs"
stage="dist/macos/OBS Tab Player $version"
cp -R "$bundle" "$stage/"
cp docs/demo/*.gp5 "$stage/demo-tabs/"
cp LICENSE "$stage/LICENSE.txt"
cp plugin/packaging/macos/README.txt plugin/packaging/macos/Install.command plugin/packaging/macos/Uninstall.command "$stage/"
chmod +x "$stage/Install.command" "$stage/Uninstall.command"
(cd dist/macos && ditto -c -k --keepParent "OBS Tab Player $version" "../obs-tab-player-$version-macos-universal.zip")
ls -l dist/*macos*
