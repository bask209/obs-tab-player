// SPDX-License-Identifier: Apache-2.0
// Hard limits applied to everything read from files. Tab files are untrusted input:
// these keep memory use and work bounded no matter what a file claims.
#pragma once

#include <cstddef>

namespace tab::limits {

constexpr size_t kMaxFileBytes = 64u << 20;         // largest tab file accepted
constexpr size_t kMaxInflatedBytes = 128u << 20;    // largest decompressed GP7/GPX content
constexpr int kMaxBars = 20000;
constexpr int kMaxTracks = 64;
constexpr int kMaxStrings = 10;
constexpr int kMaxBeatsPerVoice = 4000;
constexpr int kMaxBarOccurrences = 20000;           // bars after expanding repeats
constexpr size_t kMaxNoteEvents = 400000;           // playback events per song
constexpr double kMinBpm = 10, kMaxBpm = 1000;
constexpr float kMaxBendSemis = 24;
constexpr int kMaxFret = 99;
constexpr size_t kMaxXmlDepth = 256;

} // namespace tab::limits
