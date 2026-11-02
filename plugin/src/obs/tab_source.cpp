// SPDX-License-Identifier: Apache-2.0
// "Tab Player" OBS source. A worker thread renders the card, the text panel and the
// tab (in 1024 px tiles, only where visible) with Direct2D; the render thread only uploads
// finished images and draws a few textured quads plus the cursor. Per-frame cost is a
// handful of GPU draws and no allocation.
#include "tab_source.hpp"
#include "engine.hpp"

#include "core/layout.hpp"
#include "render/renderer.hpp"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/platform.h>
#include <util/threading.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace tabobs {
namespace {

constexpr int kTile = 1024;
constexpr size_t kMaxTiles = 24;

struct ViewModel {
	uint64_t gen = 0;
	std::shared_ptr<const Piece> piece;
	tab::Layout layout;
	tab::Theme theme;
	Settings settings;
	int width = 0, height = 0;
	float pad = 10, panelH = 0;
	float sheetX = 0, sheetY = 0, sheetW = 0, sheetH = 0;
	float contentY = 0; // vertical centring of short tabs
	struct Box { float x, y, w, h; };
	std::map<int, std::vector<Box>> noteBoxes; // beatKey -> boxes
};

struct Upload {
	enum Kind { Card, Panel, Tile, Pill, Count } kind;
	uint64_t gen;
	int tx = 0, ty = 0, count = 0;
	tab::Image image;
};

struct Tex {
	gs_texture_t *tex = nullptr;
	int w = 0, h = 0;
};

class TabSource {
public:
	TabSource(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		update(settings);
		worker_ = std::thread([this] { workerLoop(); });
		Engine::get().registerSource(source_);
	}

	~TabSource()
	{
		Engine::get().unregisterSource(source_);
		{
			std::lock_guard<std::mutex> lock(mu_);
			quit_ = true;
		}
		cv_.notify_all();
		if (worker_.joinable())
			worker_.join();
		obs_enter_graphics();
		freeTex(card_), freeTex(panel_), freeTex(pill_);
		for (auto &t : tiles_)
			freeTex(t.second);
		for (auto &c : counts_)
			freeTex(c.second);
		obs_leave_graphics();
	}

	void update(obs_data_t *settings)
	{
		int w = int(obs_data_get_int(settings, "width"));
		int h = int(obs_data_get_int(settings, "height"));
		std::lock_guard<std::mutex> lock(mu_);
		width_ = std::clamp(w, 160, 3840);
		height_ = std::clamp(h, 90, 2160);
		dirty_ = true;
		cv_.notify_all();
	}

	uint32_t width() const { return uint32_t(width_.load()); }
	uint32_t height() const { return uint32_t(height_.load()); }

	void tick()
	{
		uint64_t v = Engine::get().version();
		if (v != seenVersion_) {
			seenVersion_ = v;
			std::lock_guard<std::mutex> lock(mu_);
			dirty_ = true;
			cv_.notify_all();
		}
	}

	void render();

private:
	void freeTex(Tex &t)
	{
		if (t.tex)
			gs_texture_destroy(t.tex);
		t = Tex();
	}
	void workerLoop();
	void buildModel(tab::Renderer &r);

	obs_source_t *source_;
	std::atomic<int> width_{1920}, height_{400};
	uint64_t seenVersion_ = 0;

	// worker side
	std::thread worker_;
	std::mutex mu_;
	std::condition_variable cv_;
	bool quit_ = false, dirty_ = true;
	std::set<std::pair<int, int>> wantTiles_; // requested by the render thread
	std::set<int> wantCounts_;
	std::shared_ptr<const ViewModel> model_; // latest model (shared with render thread)
	std::vector<Upload> uploads_;

	// render-thread side
	uint64_t texGen_ = 0;
	Tex card_, panel_, pill_;
	std::map<std::pair<int, int>, Tex> tiles_;
	std::map<int, Tex> counts_;
	std::vector<std::pair<int, int>> want_; // reused every frame (no allocation)
	struct TexDraw {
		gs_texture_t *tex;
		float x, y, w, h;          // destination (w/h < 0: natural size)
		uint32_t sx, sy, sw, sh;   // source sub-region when sw > 0
	};
	struct RectDraw {
		float x, y, w, h;
		struct vec4 color;
	};
	std::vector<TexDraw> under_, over_; // textured passes below / above the cursor
	std::vector<RectDraw> rects_;
	std::vector<Upload> pending_;       // uploads waiting for a later frame
	gs_effect_t *texEffect_ = nullptr, *solidEffect_ = nullptr;
	gs_eparam_t *imageParam_ = nullptr, *colorParam_ = nullptr;
	void queueTex(std::vector<TexDraw> &q, const Tex &t, float x, float y, float w = -1, float h = -1);
	void queueTexClipped(std::vector<TexDraw> &q, const Tex &t, float x, float y, float cx0, float cy0, float cx1, float cy1);
	void queueRect(float x, float y, float w, float h, const tab::Rgba &c);
	void flushTex(std::vector<TexDraw> &q);
	void flushRects();
	float scrollY_ = 0, scrollYTarget_ = 0;
	uint64_t lastFrameNs_ = 0;
};

// ------------------------------------------------------------------ worker

void TabSource::buildModel(tab::Renderer &r)
{
	Engine &e = Engine::get();
	auto m = std::make_shared<ViewModel>();
	m->settings = e.settings();
	m->piece = e.piece();
	m->width = width_.load();
	m->height = height_.load();
	m->theme = tab::themeByName(m->settings.theme);
	tab::applyFont(m->theme, m->settings.font);
	m->theme.colorStrings = m->settings.colorStrings;
	m->theme.card.a *= float(m->settings.background);
	if (m->theme.card.a < 0.4f) {
		// floating over video: outline everything in the colour opposite to the ink
		float lum = 0.2126f * m->theme.ink.r + 0.7152f * m->theme.ink.g + 0.0722f * m->theme.ink.b;
		m->theme.outlineText = true;
		m->theme.outline = lum > 0.5f ? tab::Rgba{0, 0, 0, 0.8f} : tab::Rgba{1, 1, 1, 0.85f};
		// lighter lines in the ink colour: the halo already separates them from the video
		m->theme.line = m->theme.ink;
		m->theme.line.a = 0.7f;
		m->theme.muted = m->theme.ink;
		m->theme.muted.a = 0.9f;
		m->theme.cursorBar.a *= 0.5f; // a lighter bar band, so the video shows through
	}
	{
		std::lock_guard<std::mutex> lock(mu_);
		m->gen = (model_ ? model_->gen : 0) + 1;
	}
	const float W = float(m->width), H = float(m->height);
	// One line of tab is a strip: size the text by its height too, and keep the panel to one row.
	const bool strip = m->settings.horizontal;
	const float ui = std::clamp(strip ? std::min(W / 960.f, H / 300.f) : W / 960.f, 0.6f, 3.f);
	m->pad = std::round(10 * ui);
	std::vector<Upload> out;

	// text panel
	tab::PanelContent pc;
	pc.scale = ui;
	const Piece *p = m->piece.get();
	auto status = e.status();
	if (!p || p->fileId.empty()) {
		pc.title = "Pick a tab in the Tab Player dock";
	} else {
		pc.title = tab::displayTitle(p->item);
		if (!strip)
			pc.subtitle = p->score.subTitle.empty() ? p->score.artist : p->score.subTitle;
		if (!p->error.empty())
			pc.lines.push_back({"Cannot open this file:", p->error});
		double base = status.baseTempo > 0 ? status.baseTempo : p->score.tempo;
		char b[64];
		std::snprintf(b, sizeof b, "%d BPM \xC2\xB7 %.2f\xC3\x97", int(std::lround(base * m->settings.speed)), m->settings.speed);
		pc.badge1 = b;
		std::string flags;
		if (p->notes.tempoMin)
			flags = std::to_string(p->notes.tempoMin) + "\xE2\x80\x93" + std::to_string(p->notes.tempoMax);
		if (m->settings.loop)
			flags += std::string(flags.empty() ? "" : " \xC2\xB7 ") + (m->settings.loopFrom ? "loop " + std::to_string(m->settings.loopFrom) + "\xE2\x80\x93" + std::to_string(std::max(m->settings.loopFrom, m->settings.loopTo)) : "loop");
		if (m->settings.metronome)
			flags += " \xC2\xB7 click";
		pc.badge2 = flags;
		if (m->settings.showNotes && !strip) {
			if (!p->notes.description.empty())
				pc.lines.push_back({"", p->notes.description});
			if (!p->notes.tip.empty())
				pc.lines.push_back({"Tip:", p->notes.tip});
			if (p->notes.description.empty() && p->notes.tip.empty())
				for (size_t i = 0; i < p->notes.lines.size() && i < 2; i++)
					pc.lines.push_back({"", p->notes.lines[i]});
		}
	}
	if (m->settings.showPanel || !p || p->fileId.empty()) {
		int used = 0;
		tab::Image panel = r.renderPanel(pc, m->theme, int(W - 2 * m->pad), used);
		m->panelH = float(used);
		out.push_back({Upload::Panel, m->gen, 0, 0, 0, std::move(panel)});
	}

	// sheet area and auto-fit scale
	m->sheetX = m->pad;
	m->sheetY = m->pad + (m->panelH > 0 ? m->panelH + m->pad * 0.8f : 0);
	m->sheetW = W - 2 * m->pad;
	m->sheetH = std::max(0.f, H - m->sheetY - m->pad);
	if (m->theme.card.a > 0.004f) // fully floating: no card at all, nothing to upload or draw
		out.push_back({Upload::Card, m->gen, 0, 0, 0, r.renderCard(int(m->sheetW), int(std::max(1.f, m->sheetH)), m->theme, 14 * ui)});

	if (p && !p->score.tracks.empty() && m->sheetH > 20) {
		tab::LayoutOptions o;
		o.horizontal = m->settings.horizontal;
		o.rhythm = m->settings.rhythm;
		o.barNumbers = m->settings.barNumbers;
		int track = std::clamp(m->settings.track, 0, int(p->score.tracks.size()) - 1);
		auto layoutAt = [&](float s) {
			o.scale = s;
			o.width = m->sheetW / 1.f;
			return tab::layoutTrack(p->score, track, o);
		};
		float scale;
		if (m->settings.zoom > 0) {
			scale = float(m->settings.zoom) * ui;
		} else if (o.horizontal) {
			auto one = layoutAt(1);
			scale = std::clamp(m->sheetH / std::max(1.f, one.height), 0.5f, 4.f);
		} else {
			// Largest scale whose rows fit the height. Height is not monotonic in scale (rows
			// wrap), so scan the range instead of bisecting. ~70 layouts of a few us each.
			scale = 0.6f;
			for (float s = 4.f; s >= 0.6f; s -= 0.05f)
				if (layoutAt(s).height <= m->sheetH) {
					scale = s;
					break;
				}
		}
		m->layout = layoutAt(scale);
		if (m->layout.height < m->sheetH && !o.horizontal)
			m->contentY = std::floor((m->sheetH - m->layout.height) / 2);
		else if (o.horizontal && m->layout.height < m->sheetH)
			m->contentY = std::floor((m->sheetH - m->layout.height) / 2);
		// boxes behind the notes of each beat, for the "lit" highlight
		for (auto &pr : m->layout.prims)
			if (pr.type == tab::Prim::Text && pr.beatKey >= 0 && (pr.role == tab::Role::Note || pr.role == tab::Role::NoteSoft)) {
				float w = float(pr.text.size()) * pr.size * 0.62f + pr.size * 0.55f, h = pr.size * 1.25f;
				m->noteBoxes[pr.beatKey].push_back({pr.x0 - w / 2, pr.y0 - h / 2, w, h});
			}
		float pillH = 13.f * scale * 1.25f;
		tab::Rgba pill = m->theme.played;
		pill.a *= 0.45f;
		out.push_back({Upload::Pill, m->gen, 0, 0, 0, r.renderPill(int(std::ceil(pillH * 2)), int(std::ceil(pillH)), pill)});
	}

	std::lock_guard<std::mutex> lock(mu_);
	model_ = m;
	for (auto &u : out)
		uploads_.push_back(std::move(u));
	wantTiles_.clear();
	wantCounts_.clear();
}

void TabSource::workerLoop()
{
	os_set_thread_name("tab-player-render");
	tab::Renderer renderer; // draws into images on this thread (Qt's painter)
	if (!renderer.ok())
		blog(LOG_WARNING, "[tab-player] Qt is not available; the tab cannot be drawn");
	std::set<std::pair<int, int>> done;
	std::set<int> doneCounts;
	uint64_t doneGen = 0;
	while (true) {
		std::set<std::pair<int, int>> want;
		std::set<int> counts;
		bool rebuild;
		{
			std::unique_lock<std::mutex> lock(mu_);
			cv_.wait(lock, [&] {
				if (quit_ || dirty_)
					return true;
				for (auto &t : wantTiles_)
					if (!done.count(t))
						return true;
				for (int c : wantCounts_)
					if (!doneCounts.count(c))
						return true;
				return false;
			});
			if (quit_)
				return;
			rebuild = dirty_;
			dirty_ = false;
			want = wantTiles_;
			counts = wantCounts_;
		}
		if (!renderer.ok())
			continue;
		try {
			if (rebuild) {
				buildModel(renderer);
				done.clear();
				doneCounts.clear();
				continue;
			}
			std::shared_ptr<const ViewModel> m;
			{
				std::lock_guard<std::mutex> lock(mu_);
				m = model_;
			}
			if (!m)
				continue;
			if (m->gen != doneGen) {
				done.clear();
				doneCounts.clear();
				doneGen = m->gen;
			}
			for (auto &t : want) {
				if (done.count(t))
					continue;
				int x = t.first * kTile, y = t.second * kTile;
				int w = std::min(kTile, int(std::ceil(m->layout.width)) - x), h = std::min(kTile, int(std::ceil(m->layout.height)) - y);
				Upload u{Upload::Tile, m->gen, t.first, t.second, 0, {}};
				if (w > 0 && h > 0)
					u.image = renderer.renderSheet(m->layout, m->theme, x, y, w, h);
				done.insert(t);
				std::lock_guard<std::mutex> lock(mu_);
				uploads_.push_back(std::move(u));
			}
			for (int c : counts) {
				if (doneCounts.count(c))
					continue;
				float size = std::clamp(m->sheetH * 0.45f, 24.f, 240.f);
				tab::Rgba bubble = m->theme.card;
				bubble.a = std::max(bubble.a, 0.85f);
				if (m->theme.card.a < 0.4f)
					bubble = {0, 0, 0, 0.55f};
				Upload u{Upload::Count, m->gen, 0, 0, c, renderer.renderLabel(std::to_string(c), size, m->theme.accent, true, &bubble)};
				doneCounts.insert(c);
				std::lock_guard<std::mutex> lock(mu_);
				uploads_.push_back(std::move(u));
			}
		} catch (const std::exception &ex) {
			blog(LOG_WARNING, "[tab-player] render error: %s", ex.what());
		} catch (...) {
			blog(LOG_WARNING, "[tab-player] render error");
		}
	}
}

// ------------------------------------------------------------------ render thread

// Draws are queued and flushed in three batched passes (textures under the cursor, the
// cursor rectangles, textures over it): one shader pass each instead of one per quad.
void TabSource::queueTex(std::vector<TexDraw> &q, const Tex &t, float x, float y, float w, float h)
{
	if (t.tex)
		q.push_back({t.tex, x, y, w, h, 0, 0, 0, 0});
}

// Clipping by sub-region instead of a scissor rect keeps it correct under scene transforms.
void TabSource::queueTexClipped(std::vector<TexDraw> &q, const Tex &t, float x, float y, float cx0, float cy0, float cx1, float cy1)
{
	if (!t.tex)
		return;
	float x0 = std::max(x, cx0), y0 = std::max(y, cy0), x1 = std::min(x + float(t.w), cx1), y1 = std::min(y + float(t.h), cy1);
	if (x1 - x0 < 1 || y1 - y0 < 1)
		return;
	uint32_t sx = uint32_t(std::lround(x0 - x)), sy = uint32_t(std::lround(y0 - y));
	uint32_t w = std::min(uint32_t(std::lround(x1 - x0)), uint32_t(t.w) - sx), h = std::min(uint32_t(std::lround(y1 - y0)), uint32_t(t.h) - sy);
	if (w && h)
		q.push_back({t.tex, std::round(x0), std::round(y0), -1, -1, sx, sy, w, h});
}

void TabSource::queueRect(float x, float y, float w, float h, const tab::Rgba &c)
{
	if (w <= 0 || h <= 0 || c.a <= 0)
		return;
	RectDraw r{x, y, w, h, {}};
	// premultiplied, matching the blend mode used for everything this source draws
	vec4_set(&r.color, c.r * c.a, c.g * c.a, c.b * c.a, c.a);
	rects_.push_back(r);
}

void TabSource::flushTex(std::vector<TexDraw> &q)
{
	if (q.empty())
		return;
	while (gs_effect_loop(texEffect_, "Draw")) {
		for (auto &d : q) {
			gs_effect_set_texture(imageParam_, d.tex);
			gs_matrix_push();
			gs_matrix_translate3f(d.x, d.y, 0);
			if (d.sw)
				gs_draw_sprite_subregion(d.tex, 0, d.sx, d.sy, d.sw, d.sh);
			else
				gs_draw_sprite(d.tex, 0, d.w < 0 ? 0 : uint32_t(std::max(1.f, d.w)), d.h < 0 ? 0 : uint32_t(std::max(1.f, d.h)));
			gs_matrix_pop();
		}
	}
	q.clear();
}

void TabSource::flushRects()
{
	if (rects_.empty())
		return;
	while (gs_effect_loop(solidEffect_, "Solid")) {
		for (auto &r : rects_) {
			gs_effect_set_vec4(colorParam_, &r.color);
			gs_matrix_push();
			gs_matrix_translate3f(r.x, r.y, 0);
			gs_draw_sprite(nullptr, 0, uint32_t(std::max(1.f, r.w)), uint32_t(std::max(1.f, r.h)));
			gs_matrix_pop();
		}
	}
	rects_.clear();
}

void TabSource::render()
{
	const uint64_t startNs = os_gettime_ns();
	struct Timer {
		uint64_t start;
		~Timer() { Engine::get().addRenderTime(os_gettime_ns() - start); }
	} timer{startNs};
	std::shared_ptr<const ViewModel> m;
	{
		std::lock_guard<std::mutex> lock(mu_);
		m = model_;
		if (!uploads_.empty()) {
			for (auto &u : uploads_)
				pending_.push_back(std::move(u));
			uploads_.clear();
		}
	}
	if (!m)
		return;
	if (!texEffect_) {
		texEffect_ = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		solidEffect_ = obs_get_base_effect(OBS_EFFECT_SOLID);
		imageParam_ = gs_effect_get_param_by_name(texEffect_, "image");
		colorParam_ = gs_effect_get_param_by_name(solidEffect_, "color");
	}
	// textures from an older model are useless now
	if (m->gen != texGen_) {
		if (m->theme.card.a <= 0.004f)
			freeTex(card_);
		for (auto &t : tiles_)
			freeTex(t.second);
		tiles_.clear();
		for (auto &c : counts_)
			freeTex(c.second);
		counts_.clear();
		texGen_ = m->gen;
	}
	size_t uploaded = 0, consumed = 0;
	for (; consumed < pending_.size() && uploaded < 2; consumed++) {
		auto &u = pending_[consumed];
		if (u.gen != m->gen || u.image.bgra.empty() && u.kind != Upload::Tile)
			continue;
		uploaded++;
		Tex t;
		if (!u.image.bgra.empty()) {
			const uint8_t *data = u.image.bgra.data();
			t.tex = gs_texture_create(uint32_t(u.image.width), uint32_t(u.image.height), GS_BGRA, 1, &data, 0);
			t.w = u.image.width, t.h = u.image.height;
		}
		switch (u.kind) {
		case Upload::Card: freeTex(card_); card_ = t; break;
		case Upload::Panel: freeTex(panel_); panel_ = t; break;
		case Upload::Pill: freeTex(pill_); pill_ = t; break;
		case Upload::Count: freeTex(counts_[u.count]); counts_[u.count] = t; break;
		case Upload::Tile: freeTex(tiles_[{u.tx, u.ty}]); tiles_[{u.tx, u.ty}] = t; break;
		}
	}
	if (consumed)
		pending_.erase(pending_.begin(), pending_.begin() + std::ptrdiff_t(consumed));

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	if (m->panelH > 0)
		queueTex(under_, panel_, m->pad, m->pad);
	if (m->sheetH <= 1 || !m->piece) {
		flushTex(under_);
		gs_blend_state_pop();
		return;
	}
	queueTex(under_, card_, m->sheetX, m->sheetY);

	const auto &L = m->layout;
	const tab::Timeline &tl = m->piece->timeline;
	uint64_t now = obs_get_video_frame_time();
	int countIn = 0;
	double time = Engine::get().positionAtTime(now, &countIn);
	double dt = lastFrameNs_ ? double(now - lastFrameNs_) / 1e9 : 0;
	lastFrameNs_ = now;

	// where are we?
	const tab::BeatStop *b = nullptr, *nb = nullptr;
	if (!tl.beats.empty()) {
		auto it = std::upper_bound(tl.beats.begin(), tl.beats.end(), time + 1e-6, [](double t, const tab::BeatStop &s) { return t < s.t; });
		size_t i = it == tl.beats.begin() ? 0 : size_t(it - tl.beats.begin()) - 1;
		b = &tl.beats[i];
		nb = i + 1 < tl.beats.size() ? &tl.beats[i + 1] : nullptr;
	}
	const tab::BarGeo *bar = b && size_t(b->bar) < L.bars.size() ? &L.bars[size_t(b->bar)] : nullptr;
	const tab::SysGeo *sys = bar && size_t(bar->sys) < L.systems.size() ? &L.systems[size_t(bar->sys)] : nullptr;
	float cursorX = 0;
	int key = -1;
	if (bar && sys) {
		key = b->bar * 4096 + b->beat;
		auto x0It = L.beats.find(key);
		float x0 = x0It != L.beats.end() ? x0It->second : bar->x0;
		float x1 = bar->x1;
		if (nb) {
			const tab::BarGeo *nbar = size_t(nb->bar) < L.bars.size() ? &L.bars[size_t(nb->bar)] : nullptr;
			auto n = L.beats.find(nb->bar * 4096 + nb->beat);
			if (nbar && nbar->sys == bar->sys && n != L.beats.end() && n->second >= x0)
				x1 = n->second;
		}
		double f = b->dur > 0 ? std::clamp((time - b->t) / b->dur, 0.0, 1.0) : 0;
		cursorX = x0 + float(f) * (x1 - x0);
	}

	// scrolling: rows glide into view; the single-line layout follows the cursor
	float scrollX = 0;
	if (m->settings.horizontal) {
		scrollX = std::clamp(cursorX - m->sheetW * 0.3f, 0.f, std::max(0.f, L.width - m->sheetW));
		scrollYTarget_ = 0;
	} else if (sys) {
		scrollYTarget_ = L.height <= m->sheetH ? 0 : std::clamp(sys->y - 4.f, 0.f, L.height - m->sheetH);
	}
	float k = dt > 0 && dt < 0.5 ? float(1 - std::exp(-dt * 10)) : 1.f;
	scrollY_ += (scrollYTarget_ - scrollY_) * k;
	if (std::abs(scrollYTarget_ - scrollY_) < 0.3f)
		scrollY_ = scrollYTarget_;

	const float ox = m->sheetX - scrollX, oy = m->sheetY + m->contentY - scrollY_;
	const float cx0 = m->sheetX, cy0 = m->sheetY, cx1 = m->sheetX + m->sheetW, cy1 = m->sheetY + m->sheetH;
	auto clipRect = [&](float x, float y, float w, float h, const tab::Rgba &c) {
		float x0 = std::max(x, cx0), y0 = std::max(y, cy0), x1 = std::min(x + w, cx1), y1 = std::min(y + h, cy1);
		queueRect(x0, y0, x1 - x0, y1 - y0, c);
	};

	// bar highlight and lit notes underneath the notation
	const float zoom = L.scale;
	if (bar && sys) {
		float top = sys->top - 10 * zoom, h = sys->bottom - sys->top + 20 * zoom;
		clipRect(ox + bar->x0, oy + top, bar->x1 - bar->x0, h, m->theme.cursorBar);
		float w = std::max(2.f, 3 * zoom * 0.8f);
		tab::Rgba glow = m->theme.cursor;
		glow.a *= 0.25f;
		clipRect(ox + cursorX - w * 2, oy + top, w * 4, h, glow);
		clipRect(ox + cursorX - w / 2, oy + top, w, h, m->theme.cursor);
		if (m->settings.highlightNotes && pill_.tex && countIn == 0 && Engine::get().isPlaying()) {
			auto boxes = m->noteBoxes.find(key);
			if (boxes != m->noteBoxes.end())
				for (auto &bx : boxes->second)
					if (ox + bx.x >= cx0 && oy + bx.y >= cy0 && ox + bx.x + bx.w <= cx1 && oy + bx.y + bx.h <= cy1)
						queueTex(over_, pill_, ox + bx.x, oy + bx.y, bx.w, bx.h);
		}
	}

	// visible tiles; ask the worker for missing ones (plus one ahead)
	auto &want = want_;
	want.clear();
	int tx0 = int(std::floor(scrollX / kTile)), tx1 = int(std::floor((scrollX + m->sheetW) / kTile)) + (m->settings.horizontal ? 1 : 0);
	int ty0 = int(std::floor(std::max(0.f, scrollY_) / kTile)), ty1 = int(std::floor((scrollY_ + m->sheetH) / kTile)) + (m->settings.horizontal ? 0 : 1);
	int maxTx = int(std::ceil(L.width / kTile)) - 1, maxTy = int(std::ceil(L.height / kTile)) - 1;
	for (int ty = std::max(0, ty0); ty <= std::min(ty1, maxTy); ty++)
		for (int tx = std::max(0, tx0); tx <= std::min(tx1, maxTx); tx++) {
			auto t = tiles_.find({tx, ty});
			if (t == tiles_.end())
				want.push_back({tx, ty});
			else
				queueTexClipped(over_, t->second, ox + float(tx * kTile), oy + float(ty * kTile), cx0, cy0, cx1, cy1);
		}
	// drop far-away tiles to keep GPU memory small
	if (tiles_.size() > kMaxTiles)
		for (auto it = tiles_.begin(); it != tiles_.end() && tiles_.size() > kMaxTiles;) {
			bool far = it->first.first < tx0 - 1 || it->first.first > tx1 + 1 || it->first.second < ty0 - 1 || it->first.second > ty1 + 1;
			if (far) {
				freeTex(it->second);
				it = tiles_.erase(it);
			} else {
				++it;
			}
		}


	if (countIn > 0) {
		auto c = counts_.find(countIn);
		if (c != counts_.end() && c->second.tex)
			queueTex(over_, c->second, m->sheetX + m->sheetW - float(c->second.w) - m->pad, m->sheetY + (m->sheetH - float(c->second.h)) / 2);
		else
			want.push_back({-1, countIn});
	}
	flushTex(under_);
	flushRects();
	flushTex(over_);
	gs_blend_state_pop();

	if (!want.empty()) {
		std::lock_guard<std::mutex> lock(mu_);
		bool changed = false;
		for (auto &t : want) {
			if (t.first == -1)
				changed |= wantCounts_.insert(t.second).second;
			else
				changed |= wantTiles_.insert(t).second;
		}
		if (changed)
			cv_.notify_all();
	}
}

// ------------------------------------------------------------------ OBS glue (no exception may escape)

template<class F> auto guard(F &&f, decltype(f()) fallback) noexcept -> decltype(f())
{
	try {
		return f();
	} catch (const std::exception &e) {
		blog(LOG_ERROR, "[tab-player] %s", e.what());
	} catch (...) {
		blog(LOG_ERROR, "[tab-player] unexpected error");
	}
	return fallback;
}

const char *sourceName(void *) { return obs_module_text("TabPlayer"); }

void *sourceCreate(obs_data_t *settings, obs_source_t *source)
{
	return guard([&]() -> void * { return new TabSource(settings, source); }, nullptr);
}

void sourceDestroy(void *data)
{
	guard([&] { delete static_cast<TabSource *>(data); return 0; }, 0);
}

void sourceUpdate(void *data, obs_data_t *settings)
{
	guard([&] { static_cast<TabSource *>(data)->update(settings); return 0; }, 0);
}

uint32_t sourceWidth(void *data) { return data ? static_cast<TabSource *>(data)->width() : 0; }
uint32_t sourceHeight(void *data) { return data ? static_cast<TabSource *>(data)->height() : 0; }

void sourceTick(void *data, float)
{
	guard([&] { static_cast<TabSource *>(data)->tick(); return 0; }, 0);
}

void sourceRender(void *data, gs_effect_t *)
{
	guard([&] { static_cast<TabSource *>(data)->render(); return 0; }, 0);
}

void sourceDefaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 400);
}

bool openDockClicked(obs_properties_t *, obs_property_t *, void *)
{
	showTabPlayerDock();
	return false;
}

obs_properties_t *sourceProperties(void *)
{
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_text(p, "hint", obs_module_text("SourceHint"), OBS_TEXT_INFO);
	obs_properties_add_int(p, "width", obs_module_text("Width"), 160, 3840, 10);
	obs_properties_add_int(p, "height", obs_module_text("Height"), 90, 2160, 10);
	obs_properties_add_button(p, "open_dock", obs_module_text("OpenDock"), openDockClicked);
	return p;
}

} // namespace

void registerTabSource()
{
	obs_source_info info = {};
	info.id = kTabSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW;
	info.icon_type = OBS_ICON_TYPE_TEXT;
	info.get_name = sourceName;
	info.create = sourceCreate;
	info.destroy = sourceDestroy;
	info.update = sourceUpdate;
	info.get_width = sourceWidth;
	info.get_height = sourceHeight;
	info.video_tick = sourceTick;
	info.video_render = sourceRender;
	info.get_defaults = sourceDefaults;
	info.get_properties = sourceProperties;
	obs_register_source(&info);

	obs_source_info click = {};
	click.id = kMetronomeSourceId;
	click.type = OBS_SOURCE_TYPE_INPUT;
	click.output_flags = OBS_SOURCE_AUDIO;
	click.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT;
	click.get_name = [](void *) { return obs_module_text("MetronomeSource"); };
	click.create = [](obs_data_t *, obs_source_t *source) -> void * {
		return guard([&]() -> void * { Engine::get().registerSource(source, true); return source; }, nullptr);
	};
	click.destroy = [](void *data) { guard([&] { Engine::get().unregisterSource(static_cast<obs_source_t *>(data)); return 0; }, 0); };
	click.get_properties = [](void *) {
		obs_properties_t *p = obs_properties_create();
		obs_properties_add_text(p, "hint", obs_module_text("MetronomeHint"), OBS_TEXT_INFO);
		return p;
	};
	obs_register_source(&click);
}

} // namespace tabobs
