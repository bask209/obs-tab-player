// SPDX-License-Identifier: Apache-2.0
// Small text helpers: Windows-1252 / UTF-8 decoding to UTF-8.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tab {

// Guitar Pro writes Windows-1252; some tools write UTF-8. Valid UTF-8 wins.
std::string decodeLegacyText(const uint8_t *p, size_t n);
bool isValidUtf8(const uint8_t *p, size_t n);
std::string trim(std::string_view s);
std::string toLower(std::string_view s);

} // namespace tab
