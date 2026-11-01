// SPDX-License-Identifier: Apache-2.0
#include "d2d_renderer.hpp"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>

using Microsoft::WRL::ComPtr;

namespace tab {
namespace {

constexpr int kMaxImageSide = 8192;

std::wstring widen(const std::string &s)
{
	if (s.empty())
		return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
	std::wstring w(size_t(std::max(n, 0)), L'\0');
	if (n > 0)
		MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
	return w;
}

Rgba hex(uint32_t rgb, float a = 1) { return {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a}; }
D2D1_COLOR_F col(const Rgba &c, float alpha = 1) { return D2D1::ColorF(c.r, c.g, c.b, c.a * alpha); }

} // namespace

// ------------------------------------------------------------------ themes

Theme themeByName(const std::string &name)
{
	Theme t;
	t.name = name;
	const std::array<Rgba, 8> bright = {hex(0xf87171), hex(0xfb923c), hex(0xfacc15), hex(0x4ade80), hex(0x38bdf8), hex(0xa78bfa), hex(0xf472b6), hex(0x94a3b8)};
	const std::array<Rgba, 8> deep = {hex(0xdc2626), hex(0xea580c), hex(0xca8a04), hex(0x16a34a), hex(0x0284c7), hex(0x7c3aed), hex(0xdb2777), hex(0x475569)};
	if (name == "light") {
		t.card = hex(0xffffff, 0.95f), t.ink = hex(0x16181d), t.line = hex(0x9aa1ad), t.muted = hex(0x6b7280), t.accent = hex(0x2563eb);
		t.cursor = hex(0x2563eb, 0.9f), t.cursorBar = hex(0xfacc15, 0.18f), t.played = hex(0x2563eb), t.strings = deep;
	} else if (name == "glass") {
		t.card = hex(0x14161e, 0.45f), t.ink = hex(0xffffff), t.line = hex(0xffffff, 0.45f), t.muted = hex(0xffffff, 0.75f), t.accent = hex(0x7dd3fc);
		t.cursor = hex(0x7dd3fc, 0.95f), t.cursorBar = hex(0x7dd3fc, 0.14f), t.played = hex(0x7dd3fc), t.outline = hex(0x000000, 0.55f), t.outlineText = true, t.strings = bright;
	} else if (name == "clear") {
		t.card = hex(0x000000, 0), t.ink = hex(0xffffff), t.line = hex(0xffffff, 0.85f), t.muted = hex(0xffffff, 0.85f), t.accent = hex(0xfde047);
		t.cursor = hex(0xfde047, 0.95f), t.cursorBar = hex(0xfde047, 0.16f), t.played = hex(0xfde047), t.outline = hex(0x000000, 0.85f), t.outlineText = true, t.strings = bright;
	} else if (name == "neon") {
		t.card = hex(0x080614, 0.85f), t.ink = hex(0xe9e7ff), t.line = hex(0xa78bfa, 0.45f), t.muted = hex(0xb8b2ff), t.accent = hex(0xf472b6);
		t.cursor = hex(0x22d3ee), t.cursorBar = hex(0x22d3ee, 0.12f), t.played = hex(0x22d3ee), t.strings = bright;
	} else if (name == "paper") {
		t.card = hex(0xfbf8f1), t.ink = hex(0x2b2620), t.line = hex(0xb3a99a), t.muted = hex(0x7c7262), t.accent = hex(0xb45309);
		t.cursor = hex(0xb45309, 0.85f), t.cursorBar = hex(0xb45309, 0.1f), t.played = hex(0xb45309), t.strings = deep, t.font = L"Georgia";
	} else { // dark (default)
		t.name = "dark";
		t.card = hex(0x0f1117, 0.88f), t.ink = hex(0xf4f6fa), t.line = hex(0x59606e), t.muted = hex(0xa3abb9), t.accent = hex(0x60a5fa);
		t.cursor = hex(0x60a5fa, 0.95f), t.cursorBar = hex(0xfacc15, 0.14f), t.played = hex(0xfbbf24), t.strings = bright;
	}
	return t;
}

std::vector<std::string> themeNames() { return {"dark", "light", "glass", "clear", "neon", "paper"}; }

void applyFont(Theme &theme, const std::string &font)
{
	if (font == "mono") theme.font = L"Cascadia Mono";
	else if (font == "serif") theme.font = L"Georgia";
	else if (font == "rounded") theme.font = L"Segoe UI Variable Display";
	else if (theme.name != "paper") theme.font = L"Segoe UI";
}

// ------------------------------------------------------------------ renderer

struct Renderer::Impl {
	bool comOwned = false;
	ComPtr<ID2D1Factory> d2d;
	ComPtr<IDWriteFactory> dw;
	ComPtr<IWICImagingFactory> wic;
	ComPtr<ID2D1StrokeStyle> dashed, round;
	std::map<std::wstring, ComPtr<IDWriteTextFormat>> formats;
	struct Metrics { float ascent = 0.9f, cap = 0.7f; };
	std::map<std::wstring, Metrics> metrics;

	Impl()
	{
		HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		comOwned = SUCCEEDED(hr); // S_OK or S_FALSE must be balanced; RPC_E_CHANGED_MODE must not
		D2D1_FACTORY_OPTIONS fo{};
		if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &fo, reinterpret_cast<void **>(d2d.GetAddressOf()))))
			d2d.Reset();
		if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown **>(dw.GetAddressOf()))))
			dw.Reset();
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
			wic.Reset();
		if (d2d) {
			float dashes[] = {3.f, 3.f};
			d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.f, D2D1_DASH_STYLE_CUSTOM),
					       dashes, 2, &dashed);
			d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &round);
		}
	}
	~Impl()
	{
		formats.clear();
		dashed.Reset(), round.Reset(), wic.Reset(), dw.Reset(), d2d.Reset();
		if (comOwned)
			CoUninitialize();
	}

	IDWriteTextFormat *format(const std::wstring &font, float size, bool bold, bool italic)
	{
		size = std::clamp(size, 2.f, 400.f);
		wchar_t key[64];
		swprintf_s(key, L"|%.2f|%d%d", double(size), bold, italic);
		std::wstring k = font + key;
		auto it = formats.find(k);
		if (it != formats.end())
			return it->second.Get();
		ComPtr<IDWriteTextFormat> f;
		if (FAILED(dw->CreateTextFormat(font.c_str(), nullptr, bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
						italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f)))
			return nullptr;
		f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
		if (formats.size() > 256)
			formats.clear();
		return (formats[k] = f).Get();
	}

	Metrics fontMetrics(const std::wstring &font, bool bold)
	{
		std::wstring k = font + (bold ? L"|b" : L"|n");
		auto it = metrics.find(k);
		if (it != metrics.end())
			return it->second;
		Metrics m;
		ComPtr<IDWriteFontCollection> coll;
		UINT32 index = 0;
		BOOL exists = FALSE;
		ComPtr<IDWriteFontFamily> family;
		ComPtr<IDWriteFont> f;
		if (SUCCEEDED(dw->GetSystemFontCollection(&coll)) && SUCCEEDED(coll->FindFamilyName(font.c_str(), &index, &exists)) && exists &&
		    SUCCEEDED(coll->GetFontFamily(index, &family)) &&
		    SUCCEEDED(family->GetFirstMatchingFont(bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &f))) {
			DWRITE_FONT_METRICS fm{};
			f->GetMetrics(&fm);
			if (fm.designUnitsPerEm) {
				m.ascent = float(fm.ascent) / fm.designUnitsPerEm;
				m.cap = float(fm.capHeight) / fm.designUnitsPerEm;
			}
		}
		return metrics[k] = m;
	}

	struct Target {
		ComPtr<IWICBitmap> bmp;
		ComPtr<ID2D1RenderTarget> rt;
		int w = 0, h = 0;
	};

	bool target(int w, int h, Target &t)
	{
		if (!d2d || !wic || w <= 0 || h <= 0 || w > kMaxImageSide || h > kMaxImageSide)
			return false;
		if (FAILED(wic->CreateBitmap(UINT(w), UINT(h), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &t.bmp)))
			return false;
		auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
		if (FAILED(d2d->CreateWicBitmapRenderTarget(t.bmp.Get(), props, &t.rt)))
			return false;
		t.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
		t.rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
		t.w = w, t.h = h;
		return true;
	}

	Image finish(Target &t)
	{
		Image img;
		if (FAILED(t.rt->EndDraw()))
			return img;
		img.width = t.w, img.height = t.h;
		img.bgra.resize(size_t(t.w) * size_t(t.h) * 4);
		WICRect r{0, 0, t.w, t.h};
		if (FAILED(t.bmp->CopyPixels(&r, UINT(t.w * 4), UINT(img.bgra.size()), img.bgra.data())))
			return Image();
		return img;
	}

	// Draws text with its cap height centred on y (align 0 start, 1 centre, 2 end).
	void text(ID2D1RenderTarget *rt, ID2D1SolidColorBrush *brush, const std::wstring &font, const std::wstring &s, float x, float y, float size, bool bold, bool italic,
		  int align, const Rgba *outline, float outlineWidth)
	{
		if (s.empty())
			return;
		IDWriteTextFormat *f = format(font, size, bold, italic);
		if (!f)
			return;
		ComPtr<IDWriteTextLayout> layout;
		if (FAILED(dw->CreateTextLayout(s.c_str(), UINT32(s.size()), f, 10000.f, size * 2, &layout)))
			return;
		DWRITE_TEXT_METRICS tm{};
		layout->GetMetrics(&tm);
		auto m = fontMetrics(font, bold);
		float left = align == 1 ? x - tm.widthIncludingTrailingWhitespace / 2 : align == 2 ? x - tm.widthIncludingTrailingWhitespace : x;
		float top = y - (m.ascent - m.cap / 2) * size;
		if (outline && outline->a > 0) {
			ComPtr<ID2D1SolidColorBrush> ob;
			rt->CreateSolidColorBrush(col(*outline), &ob);
			static const float dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
			for (auto &d : dirs)
				rt->DrawTextLayout(D2D1::Point2F(left + d[0] * outlineWidth, top + d[1] * outlineWidth), layout.Get(), ob.Get());
		}
		rt->DrawTextLayout(D2D1::Point2F(left, top), layout.Get(), brush);
	}
};

Renderer::Renderer() : d_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;
bool Renderer::ok() const { return d_->d2d && d_->dw && d_->wic; }

Image Renderer::renderSheet(const Layout &layout, const Theme &theme, int x, int y, int w, int h)
{
	Impl::Target t;
	if (!d_->target(w, h, t))
		return {};
	auto *rt = t.rt.Get();
	rt->BeginDraw();
	rt->Clear(D2D1::ColorF(0, 0, 0, 0));
	rt->SetTransform(D2D1::Matrix3x2F::Translation(float(-x), float(-y)));
	const float S = layout.scale;
	const float lw = std::clamp(S, 0.8f, 4.f);
	ComPtr<ID2D1SolidColorBrush> brush;
	rt->CreateSolidColorBrush(col(theme.ink), &brush);
	const float vx0 = float(x) - 40 * S, vx1 = float(x + w) + 40 * S, vy0 = float(y) - 40 * S, vy1 = float(y + h) + 40 * S;

	// Floating over video: a soft halo under every line and shape (the text has its own).
	const bool halo = theme.outlineText && theme.outline.a > 0;
	for (int pass = halo ? 0 : 1; pass < 2; pass++)
	for (const auto &p : layout.prims) {
		if (pass == 0 && p.type == Prim::Text)
			continue;
		// cull primitives outside the tile
		float bx0, bx1, by0, by1;
		if (p.type == Prim::Path && !p.path.empty()) {
			bx0 = bx1 = p.path[0].x[0], by0 = by1 = p.path[0].y[0];
			for (auto &op : p.path)
				for (int k = 0; k < 3; k++)
					if (op.op != 'Z' && (k == 0 || op.op == 'Q' || op.op == 'C')) {
						bx0 = std::min(bx0, op.x[k]), bx1 = std::max(bx1, op.x[k]);
						by0 = std::min(by0, op.y[k]), by1 = std::max(by1, op.y[k]);
					}
		} else {
			bx0 = std::min(p.x0, p.x1), bx1 = std::max(p.x0, p.x1), by0 = std::min(p.y0, p.y1), by1 = std::max(p.y0, p.y1);
			if (p.type == Prim::Text) bx0 -= p.size * 20, bx1 += p.size * 20, by0 -= p.size, by1 += p.size;
			if (p.type == Prim::Circle) bx0 = p.x0 - p.x1, bx1 = p.x0 + p.x1, by0 = p.y0 - p.x1, by1 = p.y0 + p.x1;
		}
		if (bx1 < vx0 || bx0 > vx1 || by1 < vy0 || by0 > vy1)
			continue;

		Rgba c = theme.ink;
		float alpha = 1;
		switch (p.role) {
		case Role::Staff: case Role::BarLine: case Role::BarThick: case Role::RepeatDot: c = theme.line; break;
		case Role::Note: case Role::NoteSoft:
			if (theme.colorStrings && p.string >= 0 && p.string < 8) c = theme.strings[size_t(p.string)];
			if (p.role == Role::NoteSoft) alpha = 0.65f;
			break;
		case Role::Clef: case Role::StringName: case Role::BarNumber: case Role::BeatText: case Role::SmallText:
		case Role::Technique: case Role::Dashed: case Role::Rest: c = theme.muted; break;
		case Role::Marker: case Role::Chord: c = theme.accent; break;
		default: break;
		}
		brush->SetColor(pass == 0 ? col(theme.outline, 0.5f * alpha) : col(c, alpha));
		const float extra = pass == 0 ? 1.6f * std::max(1.f, S) : 0.f; // halo width
		switch (p.type) {
		case Prim::Line:
			rt->DrawLine(D2D1::Point2F(p.x0, p.y0), D2D1::Point2F(p.x1, p.y1), brush.Get(), p.stroke * lw * (p.role == Role::Staff ? 0.9f : 1.f) + extra, extra > 0 ? d_->round.Get() : nullptr);
			break;
		case Prim::Rect:
			rt->FillRectangle(D2D1::RectF(p.x0 - extra / 2, p.y0 - extra / 2, p.x1 + extra / 2, p.y1 + extra / 2), brush.Get());
			break;
		case Prim::Circle:
			rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(p.x0, p.y0), p.x1 + extra / 2, p.x1 + extra / 2), brush.Get());
			break;
		case Prim::Text:
			d_->text(rt, brush.Get(), theme.font, widen(p.text), p.x0, p.y0, p.size, p.bold && (p.role != Role::Note || theme.boldNotes), p.italic, p.align,
				 theme.outlineText ? &theme.outline : nullptr, std::max(1.f, S));
			break;
		case Prim::Path: {
			ComPtr<ID2D1PathGeometry> geo;
			ComPtr<ID2D1GeometrySink> sink;
			if (FAILED(d_->d2d->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink)))
				break;
			bool open = false, closed = false;
			for (auto &op : p.path) {
				switch (op.op) {
				case 'M':
					if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
					sink->BeginFigure(D2D1::Point2F(op.x[0], op.y[0]), p.role == Role::Arrow ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
					open = true;
					break;
				case 'L': if (open) sink->AddLine(D2D1::Point2F(op.x[0], op.y[0])); break;
				case 'Q': if (open) sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(op.x[0], op.y[0]), D2D1::Point2F(op.x[1], op.y[1]))); break;
				case 'C': if (open) sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(op.x[0], op.y[0]), D2D1::Point2F(op.x[1], op.y[1]), D2D1::Point2F(op.x[2], op.y[2]))); break;
				case 'Z': if (open) { sink->EndFigure(D2D1_FIGURE_END_CLOSED); open = false; closed = true; } break;
				default: break;
				}
			}
			if (open)
				sink->EndFigure(D2D1_FIGURE_END_OPEN);
			if (FAILED(sink->Close()))
				break;
			if (p.role == Role::Arrow && closed && extra == 0)
				rt->FillGeometry(geo.Get(), brush.Get());
			else
				rt->DrawGeometry(geo.Get(), brush.Get(), (p.role == Role::Arrow ? 1.2f : p.stroke) * lw + extra, p.role == Role::Dashed && extra == 0 ? d_->dashed.Get() : d_->round.Get());
			break;
		}
		}
	}
	return d_->finish(t);
}

Image Renderer::renderCard(int w, int h, const Theme &theme, float radius)
{
	Impl::Target t;
	if (!d_->target(w, h, t))
		return {};
	t.rt->BeginDraw();
	t.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
	if (theme.card.a > 0) {
		ComPtr<ID2D1SolidColorBrush> b;
		t.rt->CreateSolidColorBrush(col(theme.card), &b);
		t.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, float(w), float(h)), radius, radius), b.Get());
	}
	return d_->finish(t);
}

Image Renderer::renderPill(int w, int h, const Rgba &color)
{
	Impl::Target t;
	if (!d_->target(w, h, t))
		return {};
	t.rt->BeginDraw();
	t.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
	ComPtr<ID2D1SolidColorBrush> b;
	t.rt->CreateSolidColorBrush(col(color), &b);
	float r = std::min(w, h) / 2.f;
	t.rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), r, r), b.Get());
	return d_->finish(t);
}

Image Renderer::renderLabel(const std::string &s, float size, const Rgba &color, bool bold, const Rgba *bubble)
{
	if (!d_->dw)
		return {};
	std::wstring ws = widen(s);
	IDWriteTextFormat *f = d_->format(L"Segoe UI", size, bold, false);
	ComPtr<IDWriteTextLayout> layout;
	if (!f || FAILED(d_->dw->CreateTextLayout(ws.c_str(), UINT32(ws.size()), f, 10000.f, size * 2, &layout)))
		return {};
	DWRITE_TEXT_METRICS tm{};
	layout->GetMetrics(&tm);
	float tw = std::ceil(tm.widthIncludingTrailingWhitespace), th = std::ceil(tm.height);
	int side = bubble ? int(std::ceil(std::max(tw, th) * 1.25f)) : 0;
	int pad = int(std::ceil(size * 0.15f)) + 2;
	int w = bubble ? side : int(tw) + pad * 2, h = bubble ? side : int(th) + pad * 2;
	Impl::Target t;
	if (!d_->target(w, h, t))
		return {};
	t.rt->BeginDraw();
	t.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
	ComPtr<ID2D1SolidColorBrush> b;
	t.rt->CreateSolidColorBrush(col(color), &b);
	if (bubble) {
		ComPtr<ID2D1SolidColorBrush> bg;
		t.rt->CreateSolidColorBrush(col(*bubble), &bg);
		t.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(w / 2.f, h / 2.f), w / 2.f - 1, h / 2.f - 1), bg.Get());
		t.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(w / 2.f, h / 2.f), w / 2.f - 2, h / 2.f - 2), b.Get(), std::max(2.f, size * 0.04f));
		t.rt->DrawTextLayout(D2D1::Point2F((w - tw) / 2.f, (h - th) / 2.f), layout.Get(), b.Get());
	} else {
		ComPtr<ID2D1SolidColorBrush> shadow;
		t.rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.35f), &shadow);
		t.rt->DrawTextLayout(D2D1::Point2F(float(pad), float(pad) + size * 0.03f), layout.Get(), shadow.Get());
		t.rt->DrawTextLayout(D2D1::Point2F(float(pad), float(pad)), layout.Get(), b.Get());
	}
	return d_->finish(t);
}

Image Renderer::renderPanel(const PanelContent &c, const Theme &theme, int width, int &usedHeight)
{
	usedHeight = 0;
	if (!d_->dw || width <= 0)
		return {};
	const float s = std::clamp(c.scale, 0.5f, 4.f);
	const float pad = 14 * s, titleSize = 21 * s, bodySize = 14.5f * s, badgeSize = 12.5f * s;
	auto layoutText = [&](const std::string &str, float size, bool bold, float maxW, ComPtr<IDWriteTextLayout> &out, bool wrap) {
		std::wstring ws = widen(str);
		IDWriteTextFormat *f = d_->format(theme.font == L"Georgia" ? L"Georgia" : L"Segoe UI", size, bold, false);
		if (!f || FAILED(d_->dw->CreateTextLayout(ws.c_str(), UINT32(ws.size()), f, std::max(10.f, maxW), size * 40, &out)))
			return DWRITE_TEXT_METRICS{};
		out->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
		if (!wrap) {
			DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
			ComPtr<IDWriteInlineObject> ellipsis;
			d_->dw->CreateEllipsisTrimmingSign(f, &ellipsis);
			out->SetTrimming(&trim, ellipsis.Get());
		}
		DWRITE_TEXT_METRICS m{};
		out->GetMetrics(&m);
		return m;
	};
	// measure
	std::vector<ComPtr<IDWriteTextLayout>> badges;
	float badgesW = 0;
	for (auto *b : {&c.badge1, &c.badge2}) {
		if (b->empty())
			continue;
		ComPtr<IDWriteTextLayout> l;
		auto m = layoutText(*b, badgeSize, true, 2000, l, false);
		badgesW += m.widthIncludingTrailingWhitespace + 22 * s + 6 * s;
		badges.push_back(l);
	}
	ComPtr<IDWriteTextLayout> title, subtitle;
	auto tm = layoutText(c.title, titleSize, true, std::max(40.f, width - pad * 2 - badgesW), title, false);
	float y = pad + tm.height;
	DWRITE_TEXT_METRICS sm{};
	if (!c.subtitle.empty()) {
		sm = layoutText(c.subtitle, bodySize * 0.92f, false, width - pad * 2, subtitle, false);
		y += sm.height;
	}
	struct Line { ComPtr<IDWriteTextLayout> l; float y; };
	std::vector<Line> lines;
	for (auto &[label, text] : c.lines) {
		ComPtr<IDWriteTextLayout> l;
		std::string full = label.empty() ? text : label + " " + text;
		auto m = layoutText(full, bodySize, false, width - pad * 2, l, true);
		if (!label.empty() && l) {
			DWRITE_TEXT_RANGE r{0, UINT32(widen(label).size())};
			l->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, r);
		}
		lines.push_back({l, y + 4 * s});
		y += m.height + 4 * s;
	}
	usedHeight = int(std::ceil(y + pad));
	Impl::Target t;
	if (!d_->target(width, usedHeight, t))
		return {};
	auto *rt = t.rt.Get();
	rt->BeginDraw();
	rt->Clear(D2D1::ColorF(0, 0, 0, 0));
	ComPtr<ID2D1SolidColorBrush> ink, muted, accent, card, shadow;
	rt->CreateSolidColorBrush(col(theme.ink), &ink);
	rt->CreateSolidColorBrush(col(theme.muted), &muted);
	rt->CreateSolidColorBrush(col(theme.accent), &accent);
	rt->CreateSolidColorBrush(col(theme.card), &card);
	rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, theme.outlineText ? 0.75f : 0.f), &shadow);
	if (theme.card.a > 0)
		rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, float(width), float(usedHeight)), 14 * s, 14 * s), card.Get());
	auto draw = [&](IDWriteTextLayout *l, float x, float yy, ID2D1SolidColorBrush *b) {
		if (!l)
			return;
		if (theme.outlineText)
			rt->DrawTextLayout(D2D1::Point2F(x + 1, yy + 1.5f), l, shadow.Get());
		rt->DrawTextLayout(D2D1::Point2F(x, yy), l, b);
	};
	draw(title.Get(), pad, pad * 0.8f, ink.Get());
	float bx = width - pad;
	for (size_t i = badges.size(); i-- > 0;) {
		DWRITE_TEXT_METRICS m{};
		badges[i]->GetMetrics(&m);
		float bw = m.widthIncludingTrailingWhitespace + 22 * s, bh = m.height + 6 * s;
		bx -= bw;
		auto rr = D2D1::RoundedRect(D2D1::RectF(bx, pad * 0.8f + 2 * s, bx + bw, pad * 0.8f + 2 * s + bh), bh / 2, bh / 2);
		rt->DrawRoundedRectangle(rr, i == 0 ? accent.Get() : muted.Get(), 1.2f * s);
		draw(badges[i].Get(), bx + 11 * s, pad * 0.8f + 5 * s, i == 0 ? accent.Get() : muted.Get());
		bx -= 6 * s;
	}
	if (subtitle)
		draw(subtitle.Get(), pad, pad * 0.8f + tm.height, muted.Get());
	for (auto &ln : lines) {
		if (ln.l) {
			DWRITE_TEXT_RANGE all{0, 100000};
			ln.l->SetDrawingEffect(nullptr, all);
		}
		draw(ln.l.Get(), pad, ln.y, ink.Get());
	}
	return d_->finish(t);
}

// ------------------------------------------------------------------ dev helpers

Image renderLayout(const Layout &layout, const Theme &theme, bool withCard)
{
	Renderer r;
	Image sheet = r.renderSheet(layout, theme, 0, 0, int(layout.width), int(layout.height));
	if (!withCard || sheet.bgra.empty())
		return sheet;
	// composite over an opaque version of the card colour (preview only)
	Rgba bg = theme.card.a > 0.05f ? theme.card : Rgba{0.18f, 0.2f, 0.26f, 1};
	for (size_t i = 0; i + 3 < sheet.bgra.size(); i += 4) {
		float a = sheet.bgra[i + 3] / 255.f;
		sheet.bgra[i + 0] = uint8_t(std::lround(sheet.bgra[i + 0] + bg.b * 255 * (1 - a)));
		sheet.bgra[i + 1] = uint8_t(std::lround(sheet.bgra[i + 1] + bg.g * 255 * (1 - a)));
		sheet.bgra[i + 2] = uint8_t(std::lround(sheet.bgra[i + 2] + bg.r * 255 * (1 - a)));
		sheet.bgra[i + 3] = 255;
	}
	return sheet;
}

bool savePng(const Image &img, const std::filesystem::path &path)
{
	if (img.bgra.empty())
		return false;
	HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	bool ok = false;
	{
		ComPtr<IWICImagingFactory> wic;
		ComPtr<IWICStream> stream;
		ComPtr<IWICBitmapEncoder> enc;
		ComPtr<IWICBitmapFrameEncode> frame;
		if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) && SUCCEEDED(wic->CreateStream(&stream)) &&
		    SUCCEEDED(stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE)) && SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
		    SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
		    SUCCEEDED(frame->SetSize(UINT(img.width), UINT(img.height)))) {
			WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppPBGRA;
			frame->SetPixelFormat(&fmt);
			ok = SUCCEEDED(frame->WritePixels(UINT(img.height), UINT(img.width * 4), UINT(img.bgra.size()), const_cast<BYTE *>(img.bgra.data()))) &&
			     SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
		}
	}
	if (SUCCEEDED(co))
		CoUninitialize();
	return ok;
}

bool saveWav(const std::vector<float> &mono, int sampleRate, const std::filesystem::path &path)
{
	std::ofstream out(path, std::ios::binary);
	if (!out)
		return false;
	auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char *>(&v), 4); };
	auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char *>(&v), 2); };
	uint32_t bytes = uint32_t(mono.size() * 2);
	out.write("RIFF", 4); u32(36 + bytes); out.write("WAVEfmt ", 8);
	u32(16); u16(1); u16(1); u32(uint32_t(sampleRate)); u32(uint32_t(sampleRate * 2)); u16(2); u16(16);
	out.write("data", 4); u32(bytes);
	for (float s : mono) {
		int16_t v = int16_t(std::lround(std::clamp(s, -1.f, 1.f) * 32767));
		u16(uint16_t(v));
	}
	return bool(out);
}

} // namespace tab
