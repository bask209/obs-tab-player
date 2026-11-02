#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# macOS: downloads and builds the OBS Studio SDK the plugin compiles against (libobs and
# obs-frontend-api from the OBS sources, plus OBS's prebuilt dependencies and Qt 6, universal).
# Every download is checked against a pinned SHA-256. Needs Xcode and CMake.
#   tools/obs-sdk.sh [dest=.deps]
# Afterwards: cmake -S plugin -B build/plugin -G Xcode -DCMAKE_PREFIX_PATH="<dest>;<dest>/qt6;<dest>/deps"
# (On Windows use tools/obs-sdk.ps1; on Linux the OBS package provides the SDK.)
set -euo pipefail

dest="${1:-.deps}"
obs_version=31.1.1
deps_version=2025-07-11
mkdir -p "$dest"
dest="$(cd "$dest" && pwd)"

fetch() { # name url sha256 dir
	local zip="$dest/$1"
	[ -f "$zip" ] || { echo "Downloading $2"; curl -fsSL -o "$zip" "$2"; }
	echo "$3  $zip" | shasum -a 256 -c - >/dev/null || { rm -f "$zip"; echo "SHA-256 mismatch for $1" >&2; exit 1; }
	if [ ! -d "$dest/$4" ]; then
		echo "Extracting $1"
		if [ "$4" = src ]; then
			(cd "$dest" && cmake -E tar xf "$zip" && mv "obs-studio-$obs_version" src)
		else
			mkdir -p "$dest/$4" && (cd "$dest/$4" && cmake -E tar xf "$zip")
		fi
	fi
}

fetch "$obs_version.zip" "https://github.com/obsproject/obs-studio/archive/refs/tags/$obs_version.zip" \
	2c8427c10b55ac6d68008df2e9a3e82f4647aaad18f105e30d4713c2de678ccf src
fetch "macos-deps-$deps_version-universal.tar.xz" "https://github.com/obsproject/obs-deps/releases/download/$deps_version/macos-deps-$deps_version-universal.tar.xz" \
	495687e63383d1a287684b6e2e9bfe246bb8f156fe265926afb1a325af1edd2a deps
fetch "macos-deps-qt6-$deps_version-universal.tar.xz" "https://github.com/obsproject/obs-deps/releases/download/$deps_version/macos-deps-qt6-$deps_version-universal.tar.xz" \
	d3f5f04b6ea486e032530bdf0187cbda9a54e0a49621a4c8ba984c5023998867 qt6

echo "Configuring libobs"
cmake -S "$dest/src" -B "$dest/obs-build" -G Xcode \
	-DOBS_CMAKE_VERSION:STRING=3.0.0 -DENABLE_PLUGINS:BOOL=OFF -DENABLE_FRONTEND:BOOL=OFF \
	-DOBS_VERSION_OVERRIDE:STRING="$obs_version" -DCMAKE_PREFIX_PATH="$dest/deps;$dest/qt6" \
	"-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DOBS_CODESIGN_IDENTITY=- >/dev/null
echo "Building libobs and obs-frontend-api"
cmake --build "$dest/obs-build" --target obs-frontend-api --config Release --parallel 2>&1 | tail -n 15
cmake --install "$dest/obs-build" --component Development --config Release --prefix "$dest" 2>&1 | tail -n 15
find "$dest" -maxdepth 6 \( -name 'libobsConfig.cmake' -o -name 'obs-frontend-apiConfig.cmake' -o -name '*.framework' \) -not -path "*/deps/*" -not -path "*/qt6/*" -not -path "*/src/*" | head -n 20
echo "OBS SDK ready: -DCMAKE_PREFIX_PATH=\"$dest;$dest/qt6;$dest/deps\""
