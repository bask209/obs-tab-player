// SPDX-License-Identifier: Apache-2.0
// Tab and panel rendering with Direct2D + DirectWrite into CPU bitmaps (premultiplied BGRA).
// Rendering happens rarely (file, size or look changes) on a worker thread; the OBS source
// only uploads the result and draws it.
#pragma once

#include "core/layout.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tab {

struct Rgba {
	float r = 0, g = 0, b = 0, a = 1;
};

struct Theme {
	std::string name;
	Rgba card, ink, line, muted, accent, cursor, cursorBar, played, outline;
	bool outlineText = false;
	std::array<Rgba, 8> strings{};
	bool colorStrings = false;
	bool boldNotes = true;
	std::wstring font = L"Segoe UI";
};

Theme themeByName(const std::string &name);
std::vector<std::string> themeNames();
void applyFont(Theme &theme, const std::string &font); // sans, rounded, mono, serif

struct Image {
	int width = 0, height = 0;
	std::vector<uint8_t> bgra; // premultiplied
};

struct PanelContent {
	std::string title, subtitle, badge1, badge2;
	std::vector<std::pair<std::string, std::string>> lines; // (label, text)
	float scale = 1;
};

class Renderer {
public:
	Renderer();
	~Renderer();
	Renderer(const Renderer &) = delete;
	Renderer &operator=(const Renderer &) = delete;
	bool ok() const;

	// Renders the region [x, y, x+w, y+h) of a layout (layout units = pixels).
	Image renderSheet(const Layout &layout, const Theme &theme, int x, int y, int w, int h);
	// Card background with rounded corners and the text panel.
	Image renderCard(int w, int h, const Theme &theme, float radius);
	Image renderPanel(const PanelContent &content, const Theme &theme, int width, int &usedHeight);
	// Rounded "pill" used to light up played notes, and big count-in digits.
	Image renderPill(int w, int h, const Rgba &color);
	Image renderLabel(const std::string &text, float size, const Rgba &color, bool bold, const Rgba *bubble = nullptr);

private:
	struct Impl;
	std::unique_ptr<Impl> d_;
};

// Developer helpers (tabtool).
Image renderLayout(const Layout &layout, const Theme &theme, bool withCard);
bool savePng(const Image &img, const std::filesystem::path &path);
bool saveWav(const std::vector<float> &mono, int sampleRate, const std::filesystem::path &path);

} // namespace tab
