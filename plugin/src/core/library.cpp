// SPDX-License-Identifier: Apache-2.0
// No std::regex here on purpose: its MSVC implementation is recursive and can overflow the
// stack on long input. Everything below is linear-time, bounded scanning.
#include "library.hpp"
#include "text.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace tab {
namespace fs = std::filesystem;

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool startsWithNoCase(std::string_view s, std::string_view prefix)
{
	if (s.size() < prefix.size())
		return false;
	for (size_t i = 0; i < prefix.size(); i++)
		if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
			return false;
	return true;
}

// "Label: value" with case-insensitive label; returns the trimmed value.
bool labelled(std::string_view line, std::string_view label, std::string &value)
{
	if (!startsWithNoCase(line, label))
		return false;
	size_t i = label.size();
	while (i < line.size() && isSpace(line[i]))
		i++;
	if (i >= line.size() || line[i] != ':')
		return false;
	value = trim(line.substr(i + 1));
	return !value.empty();
}

// Reads 2-3 digits at i, returns -1 if none.
int digitsAt(std::string_view s, size_t &i, int minDigits, int maxDigits)
{
	size_t start = i;
	int v = 0;
	while (i < s.size() && isDigit(s[i]) && int(i - start) < maxDigits)
		v = v * 10 + (s[i++] - '0');
	if (int(i - start) < minDigits || (i < s.size() && isDigit(s[i])))
		return -1;
	return v;
}

// Finds "NN-NNN" (hyphen or en dash), optionally followed by "BPM".
bool findRange(std::string_view s, bool needBpm, int &lo, int &hi)
{
	for (size_t i = 0; i < s.size(); i++) {
		if (!isDigit(s[i]) || (i && isDigit(s[i - 1])))
			continue;
		size_t k = i;
		int a = digitsAt(s, k, 2, 3);
		if (a < 0)
			continue;
		while (k < s.size() && isSpace(s[k]))
			k++;
		if (k < s.size() && s[k] == '-')
			k++;
		else if (s.substr(k, 3) == "\xE2\x80\x93")
			k += 3;
		else
			continue;
		while (k < s.size() && isSpace(s[k]))
			k++;
		int b = digitsAt(s, k, 2, 3);
		if (b < 0)
			continue;
		if (needBpm) {
			while (k < s.size() && isSpace(s[k]))
				k++;
			if (!startsWithNoCase(s.substr(k), "bpm"))
				continue;
		}
		lo = a;
		hi = b;
		return true;
	}
	return false;
}

} // namespace

NotesInfo parseNotes(const std::string &notices, const std::string &tempoLabel)
{
	NotesInfo out;
	std::istringstream in(notices);
	std::string raw;
	while (std::getline(in, raw) && out.lines.size() < 200) {
		std::string l = trim(raw);
		if (l.empty())
			continue;
		if (l.size() > 2000)
			l.resize(2000);
		out.lines.push_back(l);
		std::string v;
		if (labelled(l, "Music Description", v) || labelled(l, "Description", v))
			out.description = v;
		else if (labelled(l, "Tip", v))
			out.tip = v;
		else if (startsWithNoCase(l, "Version") || startsWithNoCase(l, "Alternative") || startsWithNoCase(l, "Primary"))
			out.extra.push_back(l);
		if (!out.tempoMin)
			findRange(l, true, out.tempoMin, out.tempoMax);
	}
	if (!out.tempoMin)
		findRange(tempoLabel, false, out.tempoMin, out.tempoMax);
	return out;
}

bool isTabFile(const std::string &fileName)
{
	auto dot = fileName.rfind('.');
	if (dot == std::string::npos)
		return false;
	auto ext = toLower(fileName.substr(dot + 1));
	return ext == "gp" || ext == "gpx" || ext == "gp3" || ext == "gp4" || ext == "gp5" || ext == "gp7" || ext == "gp8";
}

LibraryItem describeFile(const std::string &id)
{
	LibraryItem item;
	item.id = id;
	auto slash = id.rfind('/');
	std::string base = slash == std::string::npos ? id : id.substr(slash + 1);
	item.group = slash == std::string::npos ? "" : id.substr(0, slash);
	auto dot = base.rfind('.');
	item.name = dot == std::string::npos ? base : base.substr(0, dot);
	item.topic = item.name;

	// "Day 004 - Arpeggios" or "Day 004 - A - Arpeggios"
	std::string_view s = item.name;
	if (!startsWithNoCase(s, "day"))
		return item;
	size_t i = 3;
	while (i < s.size() && isSpace(s[i]))
		i++;
	int day = digitsAt(s, i, 1, 3);
	if (day < 0)
		return item;
	auto dash = [&]() {
		size_t k = i;
		while (k < s.size() && isSpace(s[k]))
			k++;
		if (k >= s.size() || s[k] != '-')
			return false;
		k++;
		while (k < s.size() && isSpace(s[k]))
			k++;
		i = k;
		return true;
	};
	if (!dash())
		return item;
	std::string alt;
	if (i + 1 < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) {
		size_t save = i;
		i++;
		if (dash())
			alt = std::string(1, char(std::toupper(static_cast<unsigned char>(s[save]))));
		else
			i = save;
	}
	std::string topic = trim(s.substr(i));
	if (topic.empty())
		return item;
	item.day = day;
	item.alt = alt;
	item.topic = topic;
	return item;
}

std::string displayTitle(const LibraryItem &item)
{
	if (item.day < 0)
		return item.name;
	char buf[16];
	std::snprintf(buf, sizeof buf, "Day %03d", item.day);
	std::string s = std::string(buf) + " \xC2\xB7 " + item.topic;
	if (!item.alt.empty())
		s += " \xC2\xB7 " + item.alt;
	return s;
}

std::vector<LibraryItem> scanLibrary(const std::string &root)
{
	std::vector<LibraryItem> items;
	std::error_code ec;
	fs::path base = fs::u8path(root);
	if (root.empty() || !fs::is_directory(base, ec))
		return items;
	fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
	for (; it != end && !ec && items.size() < 100000; it.increment(ec)) {
		auto name = it->path().filename().u8string();
		bool dir = it->is_directory(ec);
		if (!name.empty() && (name[0] == '.' || name[0] == '_')) {
			if (dir)
				it.disable_recursion_pending();
			continue;
		}
		if (dir) {
			if (it.depth() >= 6)
				it.disable_recursion_pending();
			continue;
		}
		if (!it->is_regular_file(ec) || !isTabFile(name))
			continue;
		auto rel = fs::relative(it->path(), base, ec).u8string();
		if (ec)
			continue;
		std::replace(rel.begin(), rel.end(), '\\', '/');
		items.push_back(describeFile(rel));
	}
	std::sort(items.begin(), items.end(), [](const LibraryItem &a, const LibraryItem &b) {
		if ((a.day < 0) != (b.day < 0))
			return a.day >= 0;
		if (a.day != b.day)
			return a.day < b.day;
		if (a.alt != b.alt)
			return a.alt < b.alt;
		return a.id < b.id;
	});
	return items;
}

std::vector<size_t> searchLibrary(const std::vector<LibraryItem> &items, const std::string &query)
{
	std::vector<std::string> tokens;
	{
		std::istringstream in(toLower(query));
		for (std::string t; in >> t && tokens.size() < 16;)
			if (t != "day")
				tokens.push_back(t);
	}
	auto number = [](const std::string &t, int &v) {
		if (t.empty() || t.size() > 3 || !std::all_of(t.begin(), t.end(), isDigit))
			return false;
		v = std::stoi(t);
		return true;
	};
	std::vector<size_t> out;
	for (size_t i = 0; i < items.size(); i++) {
		const auto &f = items[i];
		std::string hay;
		bool ok = true;
		for (auto &t : tokens) {
			int a = 0, b = 0;
			auto dash = t.find('-');
			if (number(t, a)) {
				ok = f.day == a;
			} else if (dash != std::string::npos && number(t.substr(0, dash), a) && number(t.substr(dash + 1), b)) {
				ok = f.day >= a && f.day <= b;
			} else {
				if (hay.empty())
					hay = toLower(f.name + " " + f.group + " " + f.title + " " + f.description);
				ok = hay.find(t) != std::string::npos;
			}
			if (!ok)
				break;
		}
		if (ok)
			out.push_back(i);
	}
	return out;
}

} // namespace tab
