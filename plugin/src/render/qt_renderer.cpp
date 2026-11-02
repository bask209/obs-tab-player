// SPDX-License-Identifier: Apache-2.0
// The renderer on every platform: Qt's painter (OBS ships Qt) drawing into images on the
// worker thread, which QPainter supports for QImage.
#include "renderer.hpp"

#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <QTextLayout>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace tab {
namespace {

constexpr int kMaxImageSide = 8192;
constexpr int kDotsPerMeter = 3780; // 96 dpi: layout units are pixels, as in the layout

QString qs(const std::string &s) { return QString::fromUtf8(s.data(), int(s.size())); }
QColor col(const Rgba &c, float alpha = 1) { return QColor::fromRgbF(c.r, c.g, c.b, std::clamp(c.a * alpha, 0.f, 1.f)); }

QFont font(const std::vector<std::string> &families, float px, bool bold, bool italic)
{
	QFont f;
	QStringList names;
	for (auto &n : families)
		names << qs(n);
	f.setFamilies(names);
	f.setPointSizeF(std::clamp(px, 2.f, 400.f) * 72.f / 96.f); // pixels at 96 dpi
	f.setWeight(bold ? QFont::Bold : QFont::Normal);
	f.setItalic(italic);
	f.setHintingPreference(QFont::PreferNoHinting); // keep the layout's exact positions
	f.setStyleStrategy(QFont::PreferAntialias);
	return f;
}

QImage canvas(int w, int h)
{
	if (w <= 0 || h <= 0 || w > kMaxImageSide || h > kMaxImageSide)
		return {};
	QImage img(w, h, QImage::Format_ARGB32_Premultiplied); // B, G, R, A in memory on every platform we build for
	if (img.isNull())
		return {};
	img.setDotsPerMeterX(kDotsPerMeter);
	img.setDotsPerMeterY(kDotsPerMeter);
	img.fill(Qt::transparent);
	return img;
}

void begin(QPainter &p)
{
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);
}

Image finish(const QImage &img)
{
	Image out;
	if (img.isNull())
		return out;
	out.width = img.width(), out.height = img.height();
	out.bgra.resize(size_t(out.width) * size_t(out.height) * 4);
	for (int y = 0; y < out.height; y++)
		std::memcpy(out.bgra.data() + size_t(y) * size_t(out.width) * 4, img.constScanLine(y), size_t(out.width) * 4);
	return out;
}

QPen pen(const QColor &c, float width, bool round, bool dashed = false)
{
	QPen p(c, std::max(0.f, width));
	p.setCapStyle(round ? Qt::RoundCap : Qt::FlatCap);
	p.setJoinStyle(round ? Qt::RoundJoin : Qt::MiterJoin);
	if (dashed)
		p.setDashPattern({3, 3}); // in stroke widths
	return p;
}

// Draws text with its cap height centred on y (align 0 start, 1 centre, 2 end).
void text(QPainter &p, const std::vector<std::string> &families, const std::string &s, float x, float y, float size, bool bold, bool italic, int align,
	  const QColor &color, const Rgba *outline, float outlineWidth)
{
	if (s.empty())
		return;
	QFont f = font(families, size, bold, italic);
	QFontMetricsF fm(f, p.device());
	QString t = qs(s);
	qreal w = fm.horizontalAdvance(t);
	qreal left = align == 1 ? x - w / 2 : align == 2 ? x - w : x;
	qreal base = y + fm.capHeight() / 2;
	p.setFont(f);
	if (outline && outline->a > 0) {
		p.setPen(col(*outline));
		static const float dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
		for (auto &d : dirs)
			p.drawText(QPointF(left + d[0] * outlineWidth, base + d[1] * outlineWidth), t);
	}
	p.setPen(color);
	p.drawText(QPointF(left, base), t);
}

// A block of text laid out once and drawn later; wrapped to a width, or one line cut with "…".
struct Block {
	std::unique_ptr<QTextLayout> layout;
	qreal width = 0, height = 0;
	void draw(QPainter &p, qreal x, qreal y) const { if (layout) layout->draw(&p, QPointF(x, y)); }
};

Block block(const std::string &str, const QFont &f, qreal maxWidth, bool wrap, const QPaintDevice *device, int boldPrefix = 0)
{
	Block b;
	QFontMetricsF fm(f, device);
	QString t = qs(str);
	if (!wrap)
		t = fm.elidedText(t, Qt::ElideRight, std::max<qreal>(10, maxWidth));
	b.layout = std::make_unique<QTextLayout>(t, f, const_cast<QPaintDevice *>(device));
	QTextOption opt;
	opt.setWrapMode(wrap ? QTextOption::WordWrap : QTextOption::NoWrap);
	b.layout->setTextOption(opt);
	if (boldPrefix > 0) {
		QTextLayout::FormatRange r;
		r.start = 0;
		r.length = boldPrefix;
		r.format.setFontWeight(QFont::DemiBold);
		b.layout->setFormats({r});
	}
	b.layout->beginLayout();
	for (;;) {
		QTextLine line = b.layout->createLine();
		if (!line.isValid())
			break;
		line.setLineWidth(wrap ? std::max<qreal>(10, maxWidth) : 1e6);
		line.setPosition(QPointF(0, b.height));
		b.height += line.height();
		b.width = std::max(b.width, line.naturalTextWidth());
	}
	b.layout->endLayout();
	return b;
}

const std::vector<std::string> &defaultFonts()
{
	static const std::vector<std::string> fonts = Theme().fonts;
	return fonts;
}

} // namespace

void ensureQtApplication()
{
	if (QGuiApplication::instance())
		return;
#if defined(__linux__)
	// no window needed, we only draw into images: without a display, use Qt's offscreen platform
	if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
		qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
	static int argc = 1;
	static char name[] = "tabtool";
	static char *argv[] = {name, nullptr};
	static QGuiApplication app(argc, argv);
}

struct Renderer::Impl {};

Renderer::Renderer() : d_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;
bool Renderer::ok() const { return QGuiApplication::instance() != nullptr; }

Image Renderer::renderSheet(const Layout &layout, const Theme &theme, int x, int y, int w, int h)
{
	QImage img = canvas(w, h);
	if (img.isNull())
		return {};
	QPainter p(&img);
	begin(p);
	p.translate(-x, -y);
	const float S = layout.scale;
	const float lw = std::clamp(S, 0.8f, 4.f);
	const float vx0 = float(x) - 40 * S, vx1 = float(x + w) + 40 * S, vy0 = float(y) - 40 * S, vy1 = float(y + h) + 40 * S;

	// Floating over video: a soft halo under every line and shape (the text has its own).
	const bool halo = theme.outlineText && theme.outline.a > 0;
	for (int pass = halo ? 0 : 1; pass < 2; pass++)
	for (const auto &pr : layout.prims) {
		if (pass == 0 && pr.type == Prim::Text)
			continue;
		// cull primitives outside the tile
		float bx0, bx1, by0, by1;
		if (pr.type == Prim::Path && !pr.path.empty()) {
			bx0 = bx1 = pr.path[0].x[0], by0 = by1 = pr.path[0].y[0];
			for (auto &op : pr.path)
				for (int k = 0; k < 3; k++)
					if (op.op != 'Z' && (k == 0 || op.op == 'Q' || op.op == 'C')) {
						bx0 = std::min(bx0, op.x[k]), bx1 = std::max(bx1, op.x[k]);
						by0 = std::min(by0, op.y[k]), by1 = std::max(by1, op.y[k]);
					}
		} else {
			bx0 = std::min(pr.x0, pr.x1), bx1 = std::max(pr.x0, pr.x1), by0 = std::min(pr.y0, pr.y1), by1 = std::max(pr.y0, pr.y1);
			if (pr.type == Prim::Text) bx0 -= pr.size * 20, bx1 += pr.size * 20, by0 -= pr.size, by1 += pr.size;
			if (pr.type == Prim::Circle) bx0 = pr.x0 - pr.x1, bx1 = pr.x0 + pr.x1, by0 = pr.y0 - pr.x1, by1 = pr.y0 + pr.x1;
		}
		if (bx1 < vx0 || bx0 > vx1 || by1 < vy0 || by0 > vy1)
			continue;

		Rgba c = theme.ink;
		float alpha = 1;
		switch (pr.role) {
		case Role::Staff: case Role::BarLine: case Role::BarThick: case Role::RepeatDot: c = theme.line; break;
		case Role::Note: case Role::NoteSoft:
			if (theme.colorStrings && pr.string >= 0 && pr.string < 8) c = theme.strings[size_t(pr.string)];
			if (pr.role == Role::NoteSoft) alpha = 0.65f;
			break;
		case Role::Clef: case Role::StringName: case Role::BarNumber: case Role::BeatText: case Role::SmallText:
		case Role::Technique: case Role::Dashed: case Role::Rest: c = theme.muted; break;
		case Role::Marker: case Role::Chord: c = theme.accent; break;
		default: break;
		}
		const QColor color = pass == 0 ? col(theme.outline, 0.5f * alpha) : col(c, alpha);
		const float extra = pass == 0 ? 1.6f * std::max(1.f, S) : 0.f; // halo width
		switch (pr.type) {
		case Prim::Line:
			p.setPen(pen(color, pr.stroke * lw * (pr.role == Role::Staff ? 0.9f : 1.f) + extra, extra > 0));
			p.drawLine(QPointF(pr.x0, pr.y0), QPointF(pr.x1, pr.y1));
			break;
		case Prim::Rect:
			p.fillRect(QRectF(QPointF(pr.x0 - extra / 2, pr.y0 - extra / 2), QPointF(pr.x1 + extra / 2, pr.y1 + extra / 2)), color);
			break;
		case Prim::Circle:
			p.setPen(Qt::NoPen);
			p.setBrush(color);
			p.drawEllipse(QPointF(pr.x0, pr.y0), pr.x1 + extra / 2, pr.x1 + extra / 2);
			p.setBrush(Qt::NoBrush);
			break;
		case Prim::Text:
			text(p, theme.fonts, pr.text, pr.x0, pr.y0, pr.size, pr.bold && (pr.role != Role::Note || theme.boldNotes), pr.italic, pr.align, color,
			     theme.outlineText ? &theme.outline : nullptr, std::max(1.f, S));
			break;
		case Prim::Path: {
			QPainterPath path;
			bool open = false, closed = false;
			for (auto &op : pr.path) {
				switch (op.op) {
				case 'M': path.moveTo(op.x[0], op.y[0]); open = true; break;
				case 'L': if (open) path.lineTo(op.x[0], op.y[0]); break;
				case 'Q': if (open) path.quadTo(op.x[0], op.y[0], op.x[1], op.y[1]); break;
				case 'C': if (open) path.cubicTo(op.x[0], op.y[0], op.x[1], op.y[1], op.x[2], op.y[2]); break;
				case 'Z': if (open) { path.closeSubpath(); open = false; closed = true; } break;
				default: break;
				}
			}
			if (pr.role == Role::Arrow && closed && extra == 0) {
				p.fillPath(path, color);
			} else {
				const bool dashed = pr.role == Role::Dashed && extra == 0;
				p.strokePath(path, pen(color, (pr.role == Role::Arrow ? 1.2f : pr.stroke) * lw + extra, !dashed, dashed));
			}
			break;
		}
		}
	}
	p.end();
	return finish(img);
}

Image Renderer::renderCard(int w, int h, const Theme &theme, float radius)
{
	QImage img = canvas(w, h);
	if (img.isNull())
		return {};
	QPainter p(&img);
	begin(p);
	if (theme.card.a > 0) {
		p.setPen(Qt::NoPen);
		p.setBrush(col(theme.card));
		p.drawRoundedRect(QRectF(0, 0, w, h), radius, radius);
	}
	p.end();
	return finish(img);
}

Image Renderer::renderPill(int w, int h, const Rgba &color)
{
	QImage img = canvas(w, h);
	if (img.isNull())
		return {};
	QPainter p(&img);
	begin(p);
	p.setPen(Qt::NoPen);
	p.setBrush(col(color));
	qreal r = std::min(w, h) / 2.0;
	p.drawRoundedRect(QRectF(0.5, 0.5, w - 1.0, h - 1.0), r, r);
	p.end();
	return finish(img);
}

Image Renderer::renderLabel(const std::string &s, float size, const Rgba &color, bool bold, const Rgba *bubble)
{
	QImage probe = canvas(1, 1);
	QFont f = font(defaultFonts(), size, bold, false);
	QFontMetricsF fm(f, &probe);
	QString t = qs(s);
	qreal tw = std::ceil(fm.horizontalAdvance(t)), th = std::ceil(fm.height());
	int side = bubble ? int(std::ceil(std::max(tw, th) * 1.25)) : 0;
	int pad = int(std::ceil(size * 0.15f)) + 2;
	int w = bubble ? side : int(tw) + pad * 2, h = bubble ? side : int(th) + pad * 2;
	QImage img = canvas(w, h);
	if (img.isNull())
		return {};
	QPainter p(&img);
	begin(p);
	p.setFont(f);
	if (bubble) {
		p.setPen(Qt::NoPen);
		p.setBrush(col(*bubble));
		p.drawEllipse(QPointF(w / 2.0, h / 2.0), w / 2.0 - 1, h / 2.0 - 1);
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(col(color), std::max(2.f, size * 0.04f)));
		p.drawEllipse(QPointF(w / 2.0, h / 2.0), w / 2.0 - 2, h / 2.0 - 2);
		p.setPen(col(color));
		p.drawText(QPointF((w - tw) / 2.0, (h - th) / 2.0 + fm.ascent()), t);
	} else {
		p.setPen(QColor::fromRgbF(0, 0, 0, 0.35f));
		p.drawText(QPointF(pad, pad + size * 0.03f + fm.ascent()), t);
		p.setPen(col(color));
		p.drawText(QPointF(pad, pad + fm.ascent()), t);
	}
	p.end();
	return finish(img);
}

Image Renderer::renderPanel(const PanelContent &c, const Theme &theme, int width, int &usedHeight)
{
	usedHeight = 0;
	if (width <= 0)
		return {};
	QImage probe = canvas(1, 1);
	const float s = std::clamp(c.scale, 0.5f, 4.f);
	const float pad = 14 * s, titleSize = 21 * s, bodySize = 14.5f * s, badgeSize = 12.5f * s;
	const auto &families = theme.fonts.front() == "Georgia" ? theme.fonts : defaultFonts();
	// measure
	std::vector<Block> badges;
	float badgesW = 0;
	for (auto *b : {&c.badge1, &c.badge2}) {
		if (b->empty())
			continue;
		Block bl = block(*b, font(families, badgeSize, true, false), 2000, false, &probe);
		badgesW += float(bl.width) + 22 * s + 6 * s;
		badges.push_back(std::move(bl));
	}
	Block title = block(c.title, font(families, titleSize, true, false), std::max(40.f, width - pad * 2 - badgesW), false, &probe);
	float y = pad + float(title.height);
	Block subtitle;
	if (!c.subtitle.empty()) {
		subtitle = block(c.subtitle, font(families, bodySize * 0.92f, false, false), width - pad * 2, false, &probe);
		y += float(subtitle.height);
	}
	struct Line {
		Block b;
		float y;
	};
	std::vector<Line> lines;
	for (auto &[label, txt] : c.lines) {
		std::string full = label.empty() ? txt : label + " " + txt;
		Block b = block(full, font(families, bodySize, false, false), width - pad * 2, true, &probe, label.empty() ? 0 : int(qs(label).size()));
		float h = float(b.height);
		lines.push_back({std::move(b), y + 4 * s});
		y += h + 4 * s;
	}
	usedHeight = int(std::ceil(y + pad));
	QImage img = canvas(width, usedHeight);
	if (img.isNull())
		return {};
	QPainter p(&img);
	begin(p);
	if (theme.card.a > 0) {
		p.setPen(Qt::NoPen);
		p.setBrush(col(theme.card));
		p.drawRoundedRect(QRectF(0, 0, width, usedHeight), 14 * s, 14 * s);
		p.setBrush(Qt::NoBrush);
	}
	auto draw = [&](const Block &b, float x, float yy, const QColor &color) {
		if (theme.outlineText) {
			p.setPen(QColor::fromRgbF(0, 0, 0, 0.75f));
			b.draw(p, x + 1, yy + 1.5f);
		}
		p.setPen(color);
		b.draw(p, x, yy);
	};
	draw(title, pad, pad * 0.8f, col(theme.ink));
	float bx = width - pad;
	for (size_t i = badges.size(); i-- > 0;) {
		const QColor bc = col(i == 0 ? theme.accent : theme.muted);
		float bw = float(badges[i].width) + 22 * s, bh = float(badges[i].height) + 6 * s;
		bx -= bw;
		p.setPen(QPen(bc, 1.2f * s));
		p.drawRoundedRect(QRectF(bx, pad * 0.8f + 2 * s, bw, bh), bh / 2, bh / 2);
		draw(badges[i], bx + 11 * s, pad * 0.8f + 5 * s, bc);
		bx -= 6 * s;
	}
	if (subtitle.layout)
		draw(subtitle, pad, pad * 0.8f + float(title.height), col(theme.muted));
	for (auto &ln : lines)
		draw(ln.b, pad, ln.y, col(theme.ink));
	p.end();
	return finish(img);
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
	QImage q(img.bgra.data(), img.width, img.height, img.width * 4, QImage::Format_ARGB32_Premultiplied);
	return q.save(QString::fromStdU16String(path.u16string()), "PNG");
}

} // namespace tab
