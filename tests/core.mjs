// SPDX-License-Identifier: Apache-2.0
// The engine under test: plugin/src/core built to WebAssembly by tools/build-web.mjs (npm test
// builds it first), the same code that runs in OBS and on the website.
import { openCore } from '../build/web/core.mjs';

export const core = await openCore();

// Opens a file and returns what the engine read: { score, timeline }.
export function open(bytes, track) {
  core.load(bytes);
  if (track != null) core.selectTrack(track);
  return { score: core.score(), timeline: core.timeline() };
}

export const loadScore = (bytes) => open(bytes).score;
export const timelineOf = (bytes) => open(bytes).timeline;
