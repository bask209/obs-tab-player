// SPDX-License-Identifier: Apache-2.0
// Detects the file format from its bytes and returns a score model.

import { isGp3to5, readGp3to5 } from './gp3to5.js';
import { isGpx, readGpxFiles } from './gpx.js';
import { isZip, readZipEntry } from './zip.js';
import { readGpif } from './gpif.js';

export const SUPPORTED_EXTENSIONS = ['gp3', 'gp4', 'gp5', 'gpx', 'gp', 'gp7', 'gp8'];

export async function loadScore(input) {
  const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
  if (isGp3to5(bytes)) return readGp3to5(bytes);
  if (isGpx(bytes)) {
    const files = readGpxFiles(bytes);
    const gpif = files.get('score.gpif');
    if (!gpif) throw new Error('GPX file has no score.gpif');
    return readGpif(new TextDecoder().decode(gpif), 'gpx');
  }
  if (isZip(bytes)) {
    const gpif = await readZipEntry(bytes, 'Content/score.gpif');
    if (!gpif) throw new Error('Zip file is not a Guitar Pro 7/8 file (no Content/score.gpif)');
    return readGpif(new TextDecoder().decode(gpif), 'gp7');
  }
  throw new Error('Unsupported or unknown file format');
}
