// SPDX-License-Identifier: Apache-2.0
#include "text.hpp"

#include <cctype>

namespace tab {

static const char16_t kCp1252High[32] = {
	0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
	0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
	0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};

static void appendUtf8(std::string &out, char32_t c)
{
	if (c < 0x80) {
		out += char(c);
	} else if (c < 0x800) {
		out += char(0xC0 | (c >> 6));
		out += char(0x80 | (c & 0x3F));
	} else {
		out += char(0xE0 | (c >> 12));
		out += char(0x80 | ((c >> 6) & 0x3F));
		out += char(0x80 | (c & 0x3F));
	}
}

bool isValidUtf8(const uint8_t *p, size_t n)
{
	size_t i = 0;
	while (i < n) {
		uint8_t c = p[i];
		int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
		if (!len || i + len > n)
			return false;
		for (int k = 1; k < len; k++)
			if ((p[i + k] & 0xC0) != 0x80)
				return false;
		i += len;
	}
	return true;
}

std::string decodeLegacyText(const uint8_t *p, size_t n)
{
	bool ascii = true;
	for (size_t i = 0; i < n; i++)
		if (p[i] > 0x7F) {
			ascii = false;
			break;
		}
	if (ascii || isValidUtf8(p, n))
		return std::string(reinterpret_cast<const char *>(p), n);
	std::string out;
	out.reserve(n + 8);
	for (size_t i = 0; i < n; i++) {
		uint8_t c = p[i];
		if (c < 0x80)
			out += char(c);
		else if (c < 0xA0)
			appendUtf8(out, kCp1252High[c - 0x80]);
		else
			appendUtf8(out, c);
	}
	return out;
}

std::string trim(std::string_view s)
{
	size_t a = 0, b = s.size();
	while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
		a++;
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
		b--;
	return std::string(s.substr(a, b - a));
}

std::string toLower(std::string_view s)
{
	std::string out(s);
	for (auto &c : out)
		if (c >= 'A' && c <= 'Z')
			c = char(c - 'A' + 'a');
	return out;
}

} // namespace tab
