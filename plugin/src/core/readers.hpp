// SPDX-License-Identifier: Apache-2.0
// Format readers and containers used by loadScore().
#pragma once

#include "model.hpp"

#include <map>
#include <string>
#include <vector>

namespace tab {

bool isGp3to5(const uint8_t *d, size_t n);
Score readGp3to5(const uint8_t *d, size_t n);

bool isGpx(const uint8_t *d, size_t n);
std::map<std::string, std::vector<uint8_t>> readGpxFiles(const uint8_t *d, size_t n);

bool isZip(const uint8_t *d, size_t n);
bool readZipEntry(const uint8_t *d, size_t n, const std::string &name, std::vector<uint8_t> &out);

// Raw DEFLATE (RFC 1951) decoder.
std::vector<uint8_t> inflateRaw(const uint8_t *d, size_t n, size_t expected = 0);

Score readGpif(const std::string &xml, const std::string &format);

} // namespace tab
