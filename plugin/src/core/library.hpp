// SPDX-License-Identifier: Apache-2.0
// Tab library: folder scan, names like "Day 004 - A - Arpeggios", search, notes parsing.
#pragma once

#include "model.hpp"

#include <string>
#include <vector>

namespace tab {

struct LibraryItem {
	std::string id;    // path relative to the library root, '/' separated, UTF-8
	std::string name;  // file name without extension
	std::string topic; // "Arpeggios" for day files, else the name
	std::string group; // sub folder
	int day = -1;      // -1 when the name has no day number
	std::string alt;   // "A", "B" ... for alternative versions
	// filled in the background from the file itself
	bool indexed = false;
	std::string title, description;
	int tempoMin = 0, tempoMax = 0;
};

struct NotesInfo {
	std::string description, tip;
	std::vector<std::string> extra, lines;
	int tempoMin = 0, tempoMax = 0;
};

NotesInfo parseNotes(const std::string &notices, const std::string &tempoLabel = {});
LibraryItem describeFile(const std::string &id);
std::string displayTitle(const LibraryItem &item);

bool isTabFile(const std::string &fileName);

// Scans a folder (UTF-8 path) recursively, sorted by day, version, name.
std::vector<LibraryItem> scanLibrary(const std::string &root);

// Indices of items matching a query such as "4", "10-20", "day 12", "sweep arpeggio".
std::vector<size_t> searchLibrary(const std::vector<LibraryItem> &items, const std::string &query);

} // namespace tab
