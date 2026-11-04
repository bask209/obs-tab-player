// SPDX-License-Identifier: Apache-2.0
#include "dock.hpp"

#include "obs/engine.hpp"
#include "obs/tab_source.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QDateTime>
#include <QListWidget>
#include <QLocale>
#include <QMainWindow>
#include <QDockWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>

namespace tabobs {
namespace {

QString qs(const std::string &s) { return QString::fromUtf8(s.c_str(), int(s.size())); }
std::string su(const QString &s) { auto b = s.toUtf8(); return std::string(b.constData(), size_t(b.size())); }
QString T(const char *key) { return QString::fromUtf8(obs_module_text(key)); }

QPointer<TabPlayerDock> g_dock;

enum Roles { DayRole = Qt::UserRole + 1, AltRole, TopicRole, RangeRole, IdRole, CurrentRole, StarRole, PlayedRole };

constexpr int kStarWidth = 22; // the star at the start of each row is its own click target
const QColor kStarColor(242, 183, 5);

// "today", "yesterday", "3 days ago", or the date
QString playedText(int64_t played)
{
	if (played <= 0)
		return {};
	QDate day = QDateTime::fromSecsSinceEpoch(played).date();
	qint64 ago = day.daysTo(QDate::currentDate());
	if (ago <= 0)
		return T("PlayedToday");
	if (ago == 1)
		return T("PlayedYesterday");
	if (ago < 7)
		return T("PlayedDaysAgo").arg(ago);
	return QLocale().toString(day, QLocale::ShortFormat);
}

// List rows: star, day number, version chip, topic, when you last played it, tempo range.
class RowDelegate : public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;
	QSize sizeHint(const QStyleOptionViewItem &o, const QModelIndex &) const override
	{
		return QSize(0, o.fontMetrics.height() + 10); // as wide as the list, never wider
	}
	void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const override
	{
		QStyleOptionViewItem opt(o);
		initStyleOption(&opt, i);
		opt.text.clear();
		QApplication::style()->drawControl(QStyle::CE_ItemViewItem, &opt, p, opt.widget);
		p->save();
		QRect r = o.rect.adjusted(8, 0, -8, 0);
		bool selected = o.state & QStyle::State_Selected;
		QColor text = selected ? o.palette.highlightedText().color() : o.palette.text().color();
		QColor muted = text;
		muted.setAlphaF(0.6f);
		QColor accent = selected ? text : o.palette.link().color();
		int day = i.data(DayRole).toInt();
		QFont f = o.font;
		int x = r.left();
		const bool starred = i.data(StarRole).toBool();
		p->setPen(starred ? kStarColor : muted);
		p->drawText(QRect(x, r.top(), kStarWidth - 6, r.height()), Qt::AlignVCenter | Qt::AlignLeft, QString::fromUtf8(starred ? "\xE2\x98\x85" : "\xE2\x98\x86"));
		x += kStarWidth - 6;
		if (day >= 0) {
			QFont b = f;
			b.setBold(true);
			p->setFont(b);
			p->setPen(accent);
			QString d = QString("%1").arg(day, 3, 10, QChar('0'));
			p->drawText(QRect(x, r.top(), 40, r.height()), Qt::AlignVCenter | Qt::AlignLeft, d);
			x += QFontMetrics(b).horizontalAdvance("000") + 10;
		}
		QString alt = i.data(AltRole).toString();
		p->setFont(f);
		if (!alt.isEmpty()) {
			QFontMetrics fm(f);
			int w = fm.horizontalAdvance(alt) + 10;
			QRect chip(x, r.center().y() - fm.height() / 2, w, fm.height());
			QColor bg = muted;
			bg.setAlphaF(0.25f);
			p->setPen(Qt::NoPen);
			p->setBrush(bg);
			p->setRenderHint(QPainter::Antialiasing);
			p->drawRoundedRect(chip, 4, 4);
			p->setPen(text);
			p->drawText(chip, Qt::AlignCenter, alt);
			x += w + 8;
		}
		QString range = i.data(RangeRole).toString();
		QString played = i.data(PlayedRole).toString();
		if (!played.isEmpty())
			range = range.isEmpty() ? played : played + "   " + range;
		// in a narrow dock the topic keeps its room: drop "played", then the tempo range
		auto widthOf = [&](const QString &s) { return s.isEmpty() ? 0 : QFontMetrics(f).horizontalAdvance(s) + 8; };
		constexpr int kTopicRoom = 80;
		if (r.right() - x - widthOf(range) < kTopicRoom)
			range = i.data(RangeRole).toString();
		if (r.right() - x - widthOf(range) < kTopicRoom)
			range.clear();
		int rw = widthOf(range);
		p->setPen(text);
		if (i.data(CurrentRole).toBool()) {
			QFont b = f;
			b.setBold(true);
			p->setFont(b);
		}
		QString topic = QFontMetrics(p->font()).elidedText(i.data(TopicRole).toString(), Qt::ElideRight, std::max(10, r.right() - x - rw));
		p->drawText(QRect(x, r.top(), r.right() - x - rw, r.height()), Qt::AlignVCenter | Qt::AlignLeft, topic);
		if (rw) {
			p->setFont(f);
			p->setPen(muted);
			p->drawText(QRect(r.right() - rw, r.top(), rw, r.height()), Qt::AlignVCenter | Qt::AlignRight, range);
		}
		p->restore();
	}
};

// One line that never makes the dock wider: long text (a folder path) is shortened in the middle.
class ElidedLabel : public QLabel {
public:
	using QLabel::QLabel;
	QSize minimumSizeHint() const override { return QSize(0, QLabel::minimumSizeHint().height()); }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		QRect r = contentsRect();
		p.setPen(palette().color(QPalette::WindowText));
		p.drawText(r, Qt::AlignVCenter | Qt::AlignLeft, fontMetrics().elidedText(text(), Qt::ElideMiddle, r.width()));
	}
};

// Its widgets in as many equal columns as fit, so a row of buttons or checkboxes wraps onto more
// lines in a narrow dock instead of widening it. Rows are balanced: four buttons go 4, 2+2 or 1+1+1+1.
class ReflowLayout : public QLayout {
public:
	ReflowLayout()
	{
		setContentsMargins(0, 0, 0, 0);
		setSpacing(6);
	}
	~ReflowLayout() override
	{
		while (QLayoutItem *it = takeAt(0))
			delete it;
	}
	void addItem(QLayoutItem *it) override { items_.append(it); }
	int count() const override { return int(items_.size()); }
	QLayoutItem *itemAt(int i) const override { return items_.value(i); }
	QLayoutItem *takeAt(int i) override { return i >= 0 && i < items_.size() ? items_.takeAt(i) : nullptr; }
	Qt::Orientations expandingDirections() const override { return Qt::Horizontal; }
	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int w) const override { return arrange(QRect(0, 0, w, 0), false); }
	QSize minimumSize() const override { return QSize(cell().width(), heightForWidth(cell().width())); }
	QSize sizeHint() const override
	{
		int n = int(shown().size());
		return QSize(n * cell().width() + std::max(0, n - 1) * spacing(), cell().height());
	}
	void setGeometry(const QRect &r) override
	{
		QLayout::setGeometry(r);
		arrange(r, true);
	}

private:
	QList<QLayoutItem *> shown() const
	{
		QList<QLayoutItem *> out;
		for (auto *it : items_)
			if (!it->isEmpty())
				out.append(it);
		return out;
	}
	QSize cell() const // the widest and the tallest item
	{
		QSize c(0, 0);
		for (auto *it : shown())
			c = c.expandedTo(it->sizeHint());
		return c;
	}
	int arrange(const QRect &rect, bool apply) const
	{
		auto items = shown();
		const int n = int(items.size());
		if (!n)
			return 0;
		QRect r = rect.marginsRemoved(contentsMargins());
		const int sp = std::max(0, spacing());
		const QSize c = cell();
		int cols = std::clamp((r.width() + sp) / std::max(1, c.width() + sp), 1, n);
		const int rows = (n + cols - 1) / cols;
		cols = (n + rows - 1) / rows;
		const int w = std::max(0, (r.width() - sp * (cols - 1)) / cols);
		if (apply)
			for (int i = 0; i < n; i++)
				items[i]->setGeometry(QRect(r.x() + (i % cols) * (w + sp), r.y() + (i / cols) * (c.height() + sp), w, c.height()));
		const QMargins m = contentsMargins();
		return m.top() + m.bottom() + rows * c.height() + (rows - 1) * sp;
	}
	QList<QLayoutItem *> items_;
};

// A combo box that can be narrower than its longest entry (a long track name).
void narrow(QComboBox *box)
{
	box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	box->setMinimumContentsLength(6);
}

// Secondary text: the theme's text colour, dimmed. (palette(mid) is nearly the background in dark themes.)
void dim(QWidget *w)
{
	QPalette p = w->palette();
	QColor c = p.color(QPalette::WindowText);
	c.setAlphaF(0.65f);
	p.setColor(QPalette::WindowText, c);
	p.setColor(QPalette::ButtonText, c);
	w->setPalette(p);
}

QPushButton *toggle(const QString &text, const QString &tip)
{
	auto *b = new QPushButton(text);
	b->setCheckable(true);
	b->setToolTip(tip);
	b->setCursor(Qt::PointingHandCursor);
	return b;
}

} // namespace

// ------------------------------------------------------------------ scene helpers

bool TabPlayerDock::sceneHasTabPlayer()
{
	obs_source_t *sceneSource = obs_frontend_get_current_scene();
	if (!sceneSource)
		return false;
	bool found = false;
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			obs_source_t *src = obs_sceneitem_get_source(item);
			if (src && strcmp(obs_source_get_unversioned_id(src), kTabSourceId) == 0) {
				*static_cast<bool *>(param) = true;
				return false;
			}
			return true;
		},
		&found);
	obs_source_release(sceneSource);
	return found;
}

namespace {

// Existing source of this type (new reference), or nullptr.
obs_source_t *findSource(const char *id)
{
	struct Find {
		const char *id;
		obs_source_t *found;
	} f{id, nullptr};
	obs_enum_sources(
		[](void *param, obs_source_t *s) {
			auto *f = static_cast<Find *>(param);
			if (strcmp(obs_source_get_unversioned_id(s), f->id) == 0) {
				f->found = obs_source_get_ref(s);
				return false;
			}
			return true;
		},
		&f);
	return f.found;
}

bool sceneHas(obs_scene_t *scene, obs_source_t *src)
{
	return scene && src && obs_scene_find_source(scene, obs_source_get_name(src)) != nullptr;
}

} // namespace

void TabPlayerDock::ensureMetronomeSource()
{
	// The clicks have their own source (own fader and tracks). Add it next to every scene that
	// shows the Tab Player, so a Tab Player added by hand gets one too.
	obs_source_t *tabSrc = findSource(kTabSourceId);
	if (!tabSrc)
		return;
	obs_source_t *click = findSource(kMetronomeSourceId);
	if (!click)
		click = obs_source_create(kMetronomeSourceId, obs_module_text("MetronomeSource"), nullptr, nullptr);
	if (click) {
		struct Ctx {
			obs_source_t *tab, *click;
		} ctx{tabSrc, click};
		obs_enum_scenes(
			[](void *param, obs_source_t *sceneSource) {
				auto *c = static_cast<Ctx *>(param);
				obs_scene_t *scene = obs_scene_from_source(sceneSource);
				if (sceneHas(scene, c->tab) && !sceneHas(scene, c->click))
					obs_scene_add(scene, c->click);
				return true;
			},
			&ctx);
		obs_source_release(click);
	}
	obs_source_release(tabSrc);
}

void TabPlayerDock::addTabPlayerToScene()
{
	obs_source_t *sceneSource = obs_frontend_get_current_scene();
	if (!sceneSource)
		return;
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	// reuse an existing Tab Player source if there is one, otherwise create it
	obs_source_t *src = findSource(kTabSourceId);
	if (!src)
		src = obs_source_create(kTabSourceId, obs_module_text("TabPlayer"), nullptr, nullptr);
	if (src) {
		obs_sceneitem_t *item = obs_scene_add(scene, src);
		obs_video_info ovi = {};
		if (item && obs_get_video_info(&ovi)) {
			// one line of tab across the bottom of the canvas, like a lower third
			float w = float(obs_source_get_width(src)), h = float(obs_source_get_height(src));
			float scale = w > 0 ? float(ovi.base_width) / w : 1.f;
			vec2 s, pos;
			vec2_set(&s, scale, scale);
			vec2_set(&pos, 0.f, float(ovi.base_height) - h * scale);
			obs_sceneitem_set_scale(item, &s);
			obs_sceneitem_set_pos(item, &pos);
		}
		obs_source_release(src);
	}
	obs_source_release(sceneSource);
	ensureMetronomeSource();
}

// ------------------------------------------------------------------ dock

TabPlayerDock::TabPlayerDock(QWidget *parent) : QWidget(parent)
{
	setAcceptDrops(true);
	buildUi();
	QPointer<TabPlayerDock> self(this);
	listener_ = Engine::get().listen([self] {
		if (self)
			QMetaObject::invokeMethod(self, "refresh", Qt::QueuedConnection);
	});
	statusTimer_ = new QTimer(this);
	statusTimer_->setInterval(200);
	connect(statusTimer_, &QTimer::timeout, this, &TabPlayerDock::refreshStatus);
	statusTimer_->start();
	refresh();
}

TabPlayerDock::~TabPlayerDock()
{
	Engine::get().unlisten(listener_);
}

void TabPlayerDock::buildUi()
{
	// Pinned at the top: setup steps, then what is playing and the transport. Below, one page at a
	// time: the tab list, practice, look and sound. The list scrolls by itself, never the panel.
	auto *v = new QVBoxLayout(this);
	v->setContentsMargins(8, 8, 8, 8);
	v->setSpacing(8);

	// --- welcome (only until set up)
	welcome_ = new QFrame(this);
	welcome_->setFrameShape(QFrame::StyledPanel);
	{
		auto *w = new QVBoxLayout(welcome_);
		auto *hi = new QLabel(T("WelcomeTitle"), welcome_);
		hi->setStyleSheet("font-weight: bold; font-size: 16px;");
		hi->setWordWrap(true);
		auto *sub = new QLabel(T("WelcomeText"), welcome_);
		sub->setWordWrap(true);
		chooseBtn_ = new QPushButton(T("ChooseFolder"), welcome_);
		addBtn_ = new QPushButton(T("AddToScene"), welcome_);
		for (auto *b : {chooseBtn_, addBtn_}) {
			b->setMinimumHeight(34);
			b->setCursor(Qt::PointingHandCursor);
		}
		w->addWidget(hi);
		w->addWidget(sub);
		w->addWidget(chooseBtn_);
		w->addWidget(addBtn_);
		connect(chooseBtn_, &QPushButton::clicked, this, &TabPlayerDock::chooseFolder);
		connect(addBtn_, &QPushButton::clicked, this, [this] {
			addTabPlayerToScene();
			refresh();
		});
	}
	v->addWidget(welcome_);

	// --- now playing (once a tab is loaded)
	playerBox_ = new QWidget(this);
	auto *pv = new QVBoxLayout(playerBox_);
	pv->setContentsMargins(0, 0, 0, 0);
	pv->setSpacing(6);
	v->addWidget(playerBox_);
	title_ = new QLabel(playerBox_);
	title_->setStyleSheet("font-weight: bold; font-size: 15px;");
	title_->setWordWrap(true);
	info_ = new QLabel(playerBox_);
	info_->setWordWrap(true);
	dim(info_);
	progress_ = new QProgressBar(playerBox_);
	progress_->setTextVisible(false);
	progress_->setMaximumHeight(6);
	progress_->setRange(0, 1000);
	progress_->setCursor(Qt::PointingHandCursor);
	progress_->setToolTip(T("SeekTip"));
	progress_->installEventFilter(this);
	starBtn_ = new QPushButton(playerBox_);
	starBtn_->setFlat(true);
	starBtn_->setCursor(Qt::PointingHandCursor);
	starBtn_->setToolTip(T("StarTip"));
	starBtn_->setStyleSheet("font-size: 18px; padding: 0 4px;");
	connect(starBtn_, &QPushButton::clicked, this, [] { Engine::get().toggleStar({}); });
	title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // wraps instead of widening the dock
	title_->setMinimumWidth(60);
	auto *titleRow = new QHBoxLayout();
	titleRow->addWidget(title_, 1);
	titleRow->addWidget(starBtn_, 0, Qt::AlignTop);
	pv->addLayout(titleRow);
	pv->addWidget(info_);
	pv->addWidget(progress_);

	// --- transport
	auto *tr = new QHBoxLayout();
	auto *style = QApplication::style();
	auto *prev = new QPushButton(style->standardIcon(QStyle::SP_MediaSkipBackward), QString(), playerBox_);
	playBtn_ = new QPushButton(playerBox_);
	auto *stopBtn = new QPushButton(style->standardIcon(QStyle::SP_MediaStop), QString(), playerBox_);
	auto *next = new QPushButton(style->standardIcon(QStyle::SP_MediaSkipForward), QString(), playerBox_);
	prev->setToolTip(T("PrevDayTip"));
	next->setToolTip(T("NextDayTip"));
	stopBtn->setToolTip(T("StopTip"));
	playBtn_->setToolTip(T("PlayTip"));
	for (auto *b : {prev, playBtn_, stopBtn, next}) {
		b->setMinimumHeight(38);
		b->setCursor(Qt::PointingHandCursor);
		QFont f = b->font();
		f.setPointSizeF(f.pointSizeF() * 1.4);
		b->setFont(f);
	}
	playBtn_->setMinimumWidth(90);
	tr->addWidget(prev);
	tr->addWidget(playBtn_, 2);
	tr->addWidget(stopBtn);
	tr->addWidget(next);
	pv->addLayout(tr);
	connect(prev, &QPushButton::clicked, this, [] { Engine::get().nextDay(-1); });
	connect(next, &QPushButton::clicked, this, [] { Engine::get().nextDay(1); });
	connect(playBtn_, &QPushButton::clicked, this, [] { Engine::get().playPause(); });
	connect(stopBtn, &QPushButton::clicked, this, [] { Engine::get().stopPlayback(); });

	pages_ = new QTabWidget(this);
	pages_->setDocumentMode(true);
	pages_->setUsesScrollButtons(true);
	v->addWidget(pages_, 1);
	v->addStretch(0); // keeps the welcome at the top while the pages are hidden

	// A settings page: its own scrolling, and a button that puts just these settings back.
	auto settingsPage = [this](const QString &name, QWidget *content, std::function<void(Settings &, const Settings &)> reset) {
		auto *scroll = new QScrollArea(pages_);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		auto *inner = new QWidget(scroll);
		auto *lay = new QVBoxLayout(inner);
		lay->setContentsMargins(0, 8, 0, 0);
		lay->setSpacing(8);
		lay->addWidget(content);
		lay->addStretch(1);
		auto *resetBtn = new QPushButton(T("ResetSection"), inner);
		resetBtn->setToolTip(T("ResetTip"));
		resetBtn->setCursor(Qt::PointingHandCursor);
		connect(resetBtn, &QPushButton::clicked, this, [reset] {
			Engine::get().update([&reset](Settings &s) { reset(s, Settings()); });
		});
		auto *row = new QHBoxLayout();
		row->addStretch(1);
		row->addWidget(resetBtn);
		lay->addLayout(row);
		scroll->setWidget(inner);
		pages_->addTab(scroll, name);
	};

	// --- page: find a tab
	auto *tabsPage = new QWidget(pages_);
	auto *lv = new QVBoxLayout(tabsPage);
	lv->setContentsMargins(0, 8, 0, 0);
	lv->setSpacing(6);
	search_ = new QLineEdit(tabsPage);
	search_->setPlaceholderText(T("SearchPlaceholder"));
	search_->setClearButtonEnabled(true);
	search_->installEventFilter(this);
	search_->setMinimumWidth(80);
	count_ = new QLabel(tabsPage);
	dim(count_);
	count_->setWordWrap(true); // never wider than the dock
	list_ = new QListWidget(tabsPage);
	list_->setItemDelegate(new RowDelegate(list_));
	list_->setUniformItemSizes(true);
	list_->setMinimumHeight(120);
	list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	list_->installEventFilter(this);
	list_->viewport()->installEventFilter(this);
	starredOnly_ = new QPushButton(QString::fromUtf8("\xE2\x98\x85 ") + T("Starred"), tabsPage);
	starredOnly_->setCheckable(true);
	starredOnly_->setCursor(Qt::PointingHandCursor);
	starredOnly_->setToolTip(T("StarredTip"));
	auto *searchRow = new QHBoxLayout();
	searchRow->addWidget(search_, 1);
	searchRow->addWidget(starredOnly_);
	lv->addLayout(searchRow);
	lv->addWidget(count_);
	lv->addWidget(list_, 1);
	footer_ = new QWidget(tabsPage);
	auto *lib = new QHBoxLayout(footer_);
	lib->setContentsMargins(0, 0, 0, 0);
	libraryLabel_ = new ElidedLabel(footer_);
	dim(libraryLabel_);
	auto *change = new QPushButton(T("Change"), footer_);
	auto *rescan = new QPushButton(T("Rescan"), footer_);
	lib->addWidget(libraryLabel_, 1);
	lib->addWidget(change);
	lib->addWidget(rescan);
	lv->addWidget(footer_);
	pages_->addTab(tabsPage, T("PageTabs"));
	connect(starredOnly_, &QPushButton::toggled, this, [](bool on) { Engine::get().update([on](Settings &s) { s.starredOnly = on; }); });
	connect(starredOnly_, &QPushButton::toggled, this, &TabPlayerDock::filter);
	connect(search_, &QLineEdit::textChanged, this, &TabPlayerDock::filter);
	connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) { openRow(list_->row(it), false); });
	connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem *it) { openRow(list_->row(it), true); });
	connect(change, &QPushButton::clicked, this, &TabPlayerDock::chooseFolder);
	connect(rescan, &QPushButton::clicked, this, [] { Engine::get().rescan(); });

	// --- page: practice (tempo, loop, count-in, metronome)
	auto *practice = new QWidget(pages_);
	auto *prv = new QVBoxLayout(practice);
	prv->setContentsMargins(0, 0, 0, 0);
	prv->setSpacing(8);
	auto *tempoBox = new QFrame(practice);
	tempoBox->setFrameShape(QFrame::StyledPanel);
	auto *tv = new QVBoxLayout(tempoBox);
	auto *th = new QHBoxLayout();
	auto *slower = new QPushButton(QString::fromUtf8("\xE2\x88\x92"), tempoBox);
	auto *faster = new QPushButton("+", tempoBox);
	slower->setToolTip(T("SlowerTip"));
	faster->setToolTip(T("FasterTip"));
	bpm_ = new QSpinBox(tempoBox);
	bpm_->setRange(10, 400);
	bpm_->setSuffix(" BPM");
	bpm_->setAlignment(Qt::AlignCenter);
	bpm_->setKeyboardTracking(false);
	QFont bf = bpm_->font();
	bf.setPointSizeF(bf.pointSizeF() * 1.5);
	bf.setBold(true);
	bpm_->setFont(bf);
	speedLabel_ = new QLabel(tempoBox);
	th->addWidget(slower);
	th->addWidget(bpm_, 1);
	th->addWidget(faster);
	th->addWidget(speedLabel_);
	tv->addLayout(th);
	speed_ = new QSlider(Qt::Horizontal, tempoBox);
	speed_->setRange(25, 200);
	speed_->setToolTip(T("SpeedTip"));
	tv->addWidget(speed_);
	auto *chips = new ReflowLayout();
	for (int pct : {50, 75, 100, 125}) {
		auto *c = new QPushButton(QString("%1%").arg(pct), tempoBox);
		c->setCursor(Qt::PointingHandCursor);
		connect(c, &QPushButton::clicked, this, [pct] { Engine::get().update([pct](Settings &s) { s.speed = pct / 100.0; }); });
		chips->addWidget(c);
	}
	tv->addLayout(chips);
	rangeRow_ = new QWidget(tempoBox);
	auto *rr = new QHBoxLayout(rangeRow_);
	rr->setContentsMargins(0, 0, 0, 0);
	auto *rl = new QLabel(T("FileRange"), rangeRow_);
	rl->setWordWrap(true);
	rr->addWidget(rl);
	for (auto &b : rangeBtns_) {
		b = new QPushButton(rangeRow_);
		b->setCursor(Qt::PointingHandCursor);
		rr->addWidget(b);
		connect(b, &QPushButton::clicked, this, [b] { Engine::get().setBpm(b->property("bpm").toDouble()); });
	}
	tv->addWidget(rangeRow_);
	prv->addWidget(tempoBox);
	connect(slower, &QPushButton::clicked, this, [] { Engine::get().speedStep(-0.05); });
	connect(faster, &QPushButton::clicked, this, [] { Engine::get().speedStep(0.05); });
	connect(bpm_, qOverload<int>(&QSpinBox::valueChanged), this, [](int bpm) { Engine::get().setBpm(bpm); });
	connect(speed_, &QSlider::valueChanged, this, [](int x) { Engine::get().update([x](Settings &s) { s.speed = x / 100.0; }); });

	auto *pr = new ReflowLayout();
	loopBtn_ = toggle(T("Loop"), T("LoopTip"));
	countBtn_ = toggle(T("CountIn"), T("CountInTip"));
	clickBtn_ = toggle(T("Metronome"), T("MetronomeTip"));
	for (auto *b : {loopBtn_, countBtn_, clickBtn_})
		pr->addWidget(b);
	prv->addLayout(pr);
	connect(loopBtn_, &QPushButton::toggled, this, [](bool on) { Engine::get().update([on](Settings &s) { s.loop = on; }); });
	connect(countBtn_, &QPushButton::toggled, this, [](bool on) { Engine::get().update([on](Settings &s) { s.countIn = on; }); });
	connect(clickBtn_, &QPushButton::toggled, this, [](bool on) { Engine::get().update([on](Settings &s) { s.metronome = on; }); });
	auto *lr = new QHBoxLayout();
	lr->addWidget(new QLabel(T("LoopBars"), practice));
	loopFrom_ = new QSpinBox(practice);
	loopTo_ = new QSpinBox(practice);
	for (auto *sb : {loopFrom_, loopTo_}) {
		sb->setRange(0, 9999);
		sb->setSpecialValueText(T("All"));
		sb->setKeyboardTracking(false);
	}
	lr->addWidget(loopFrom_);
	lr->addWidget(new QLabel(QString::fromUtf8("\xE2\x80\x93"), practice));
	lr->addWidget(loopTo_);
	lr->addStretch();
	prv->addLayout(lr);
	auto setLoop = [this] {
		int a = loopFrom_->value(), b = loopTo_->value();
		Engine::get().update([a, b](Settings &s) {
			s.loopFrom = a;
			s.loopTo = b;
			if (a || b)
				s.loop = true;
		});
	};
	connect(loopFrom_, qOverload<int>(&QSpinBox::valueChanged), this, setLoop);
	connect(loopTo_, qOverload<int>(&QSpinBox::valueChanged), this, setLoop);
	settingsPage(T("PagePractice"), practice, [](Settings &s, const Settings &d) {
		s.speed = d.speed;
		s.loop = d.loop;
		s.countIn = d.countIn;
		s.metronome = d.metronome;
		s.loopFrom = d.loopFrom;
		s.loopTo = d.loopTo;
	});

	// --- page: look
	auto *look = new QWidget(pages_);
	auto *lg = new QGridLayout(look);
	lg->setContentsMargins(0, 0, 0, 0);
	theme_ = new QComboBox(look);
	for (auto &[id, key] : std::vector<std::pair<const char *, const char *>>{{"dark", "ThemeDark"}, {"light", "ThemeLight"}, {"glass", "ThemeGlass"}, {"clear", "ThemeClear"}, {"neon", "ThemeNeon"}, {"paper", "ThemePaper"}})
		theme_->addItem(T(key), id);
	font_ = new QComboBox(look);
	for (auto &[id, key] : std::vector<std::pair<const char *, const char *>>{{"sans", "FontSans"}, {"rounded", "FontRounded"}, {"mono", "FontMono"}, {"serif", "FontSerif"}})
		font_->addItem(T(key), id);
	layout_ = new QComboBox(look);
	layout_->addItem(T("LayoutLine"), true);
	layout_->addItem(T("LayoutRows"), false);
	track_ = new QComboBox(look);
	autoSize_ = new QCheckBox(T("AutoSize"), look);
	zoom_ = new QSlider(Qt::Horizontal, look);
	zoom_->setRange(50, 300);
	colors_ = new QCheckBox(T("ColorStrings"), look);
	notes_ = new QCheckBox(T("ShowNotes"), look);
	panel_ = new QCheckBox(T("ShowPanel"), look);
	rhythm_ = new QCheckBox(T("Rhythm"), look);
	barNums_ = new QCheckBox(T("BarNumbers"), look);
	lit_ = new QCheckBox(T("LightUpNotes"), look);
	floating_ = new QCheckBox(T("Floating"), look);
	floating_->setToolTip(T("FloatingTip"));
	background_ = new QSlider(Qt::Horizontal, look);
	background_->setRange(0, 100);
	background_->setToolTip(T("BackgroundTip"));
	int row = 0;
	lg->addWidget(floating_, row++, 0, 1, 2);
	lg->addWidget(new QLabel(T("Background"), look), row, 0);
	lg->addWidget(background_, row++, 1);
	lg->addWidget(new QLabel(T("Theme"), look), row, 0);
	lg->addWidget(theme_, row++, 1);
	lg->addWidget(new QLabel(T("Font"), look), row, 0);
	lg->addWidget(font_, row++, 1);
	lg->addWidget(new QLabel(T("Layout"), look), row, 0);
	lg->addWidget(layout_, row++, 1);
	lg->addWidget(new QLabel(T("Track"), look), row, 0);
	lg->addWidget(track_, row++, 1);
	lg->addWidget(autoSize_, row, 0);
	lg->addWidget(zoom_, row++, 1);
	auto *checks = new ReflowLayout();
	for (auto *box : {colors_, lit_, panel_, notes_, rhythm_, barNums_})
		checks->addWidget(box);
	lg->addLayout(checks, row++, 0, 1, 2);
	lg->setColumnStretch(1, 1);
	for (auto *box : {theme_, font_, layout_, track_})
		narrow(box);
	settingsPage(T("Look"), look, [](Settings &s, const Settings &d) {
		s.theme = d.theme;
		s.font = d.font;
		s.horizontal = d.horizontal;
		s.zoom = d.zoom;
		s.colorStrings = d.colorStrings;
		s.rhythm = d.rhythm;
		s.barNumbers = d.barNumbers;
		s.highlightNotes = d.highlightNotes;
		s.showPanel = d.showPanel;
		s.showNotes = d.showNotes;
		s.background = d.background;
	});
	auto setStr = [](std::string Settings::*field) { return [field](const QString &id) { Engine::get().update([&](Settings &s) { s.*field = su(id); }); }; };
	connect(theme_, &QComboBox::currentIndexChanged, this, [this, setStr](int) { setStr(&Settings::theme)(theme_->currentData().toString()); });
	connect(font_, &QComboBox::currentIndexChanged, this, [this, setStr](int) { setStr(&Settings::font)(font_->currentData().toString()); });
	connect(layout_, &QComboBox::currentIndexChanged, this, [this](int) { bool h = layout_->currentData().toBool(); Engine::get().update([h](Settings &s) { s.horizontal = h; }); });
	connect(track_, &QComboBox::currentIndexChanged, this, [](int i) { if (i >= 0) Engine::get().update([i](Settings &s) { s.track = i; }); });
	connect(autoSize_, &QCheckBox::toggled, this, [this](bool on) { double z = on ? 0 : zoom_->value() / 100.0; Engine::get().update([z](Settings &s) { s.zoom = z; }); });
	connect(zoom_, &QSlider::valueChanged, this, [this](int x) { if (!autoSize_->isChecked()) Engine::get().update([x](Settings &s) { s.zoom = x / 100.0; }); });
	auto setBool = [this](QCheckBox *box, bool Settings::*field) { connect(box, &QCheckBox::toggled, this, [field](bool on) { Engine::get().update([&](Settings &s) { s.*field = on; }); }); };
	setBool(colors_, &Settings::colorStrings);
	setBool(notes_, &Settings::showNotes);
	setBool(panel_, &Settings::showPanel);
	setBool(rhythm_, &Settings::rhythm);
	setBool(barNums_, &Settings::barNumbers);
	setBool(lit_, &Settings::highlightNotes);
	connect(floating_, &QCheckBox::toggled, this, [](bool on) {
		Engine::get().update([on](Settings &s) {
			s.background = on ? 0 : 1;
			s.showPanel = !on;
		});
	});
	connect(background_, &QSlider::valueChanged, this, [](int x) { Engine::get().update([x](Settings &s) { s.background = x / 100.0; }); });

	// --- page: sound
	auto *sound = new QWidget(pages_);
	auto *sg = new QGridLayout(sound);
	sg->setContentsMargins(0, 0, 0, 0);
	tone_ = new QComboBox(sound);
	tone_->addItem(T("ToneClean"), "clean");
	tone_->addItem(T("ToneWarm"), "warm");
	tone_->addItem(T("ToneCrunch"), "crunch");
	volume_ = new QSlider(Qt::Horizontal, sound);
	clickVol_ = new QSlider(Qt::Horizontal, sound);
	volume_->setRange(0, 150);
	clickVol_->setRange(0, 150);
	offset_ = new QSlider(Qt::Horizontal, sound);
	offset_->setRange(-300, 300);
	offset_->setSingleStep(10);
	offset_->setToolTip(T("OffsetTip"));
	offsetLabel_ = new QLabel(sound);
	narrow(tone_);
	sg->setColumnStretch(1, 1);
	row = 0;
	sg->addWidget(new QLabel(T("Tone"), sound), row, 0);
	sg->addWidget(tone_, row++, 1);
	sg->addWidget(new QLabel(T("GuitarVolume"), sound), row, 0);
	sg->addWidget(volume_, row++, 1);
	sg->addWidget(new QLabel(T("ClickVolume"), sound), row, 0);
	sg->addWidget(clickVol_, row++, 1);
	sg->addWidget(offsetLabel_, row, 0);
	sg->addWidget(offset_, row++, 1);
	settingsPage(T("Sound"), sound, [](Settings &s, const Settings &d) {
		s.tone = d.tone;
		s.volume = d.volume;
		s.clickVolume = d.clickVolume;
		s.avOffsetMs = d.avOffsetMs;
	});
	connect(tone_, &QComboBox::currentIndexChanged, this, [this, setStr](int) { setStr(&Settings::tone)(tone_->currentData().toString()); });
	connect(volume_, &QSlider::valueChanged, this, [](int x) { Engine::get().update([x](Settings &s) { s.volume = x / 100.0; }); });
	connect(clickVol_, &QSlider::valueChanged, this, [](int x) { Engine::get().update([x](Settings &s) { s.clickVolume = x / 100.0; }); });
	connect(offset_, &QSlider::valueChanged, this, [](int x) { Engine::get().update([x](Settings &s) { s.avOffsetMs = x; }); });

	welcome_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
	playerBox_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

	// The mouse wheel scrolls; it never changes a slider, a number or a choice it passes over.
	for (auto *w : findChildren<QWidget *>())
		if ((qobject_cast<QAbstractSlider *>(w) && !qobject_cast<QScrollBar *>(w)) || qobject_cast<QComboBox *>(w) || qobject_cast<QAbstractSpinBox *>(w))
			w->installEventFilter(this);
}

void TabPlayerDock::chooseFolder()
{
	QString start = qs(Engine::get().settings().library);
	QString dir = QFileDialog::getExistingDirectory(this, T("ChooseFolder"), start);
	if (!dir.isEmpty())
		Engine::get().setLibrary(su(QDir::toNativeSeparators(dir)));
}

void TabPlayerDock::openRow(int row, bool play)
{
	if (row < 0 || size_t(row) >= rows_.size())
		return;
	auto *item = list_->item(row);
	if (!item)
		return;
	Engine &e = Engine::get();
	e.openFile(su(item->data(IdRole).toString()));
	if (play)
		e.restart();
}

void TabPlayerDock::fillList()
{
	auto lib = Engine::get().library();
	std::string current = Engine::get().settings().fileId;
	auto marks = Engine::get().marks();
	auto markOf = [&](const std::string &id) { auto m = marks.find(id); return m == marks.end() ? Mark() : m->second; };
	rows_ = tab::searchLibrary(lib, su(search_->text()));
	if (starredOnly_->isChecked())
		rows_.erase(std::remove_if(rows_.begin(), rows_.end(), [&](size_t i) { return !markOf(lib[i].id).starred; }), rows_.end());
	size_t starred = 0;
	for (auto &f : lib)
		starred += markOf(f.id).starred;
	QSignalBlocker block(list_);
	int keepRow = list_->currentRow();
	list_->clear();
	int currentRow = -1;
	for (size_t i : rows_) {
		const auto &f = lib[i];
		auto *it = new QListWidgetItem(list_);
		it->setData(DayRole, f.day);
		it->setData(AltRole, qs(f.alt));
		it->setData(TopicRole, qs(f.day >= 0 ? f.topic : f.name));
		it->setData(RangeRole, f.tempoMin ? QString("%1-%2").arg(f.tempoMin).arg(f.tempoMax) : QString());
		it->setData(IdRole, qs(f.id));
		it->setData(CurrentRole, f.id == current);
		it->setData(StarRole, markOf(f.id).starred);
		it->setData(PlayedRole, playedText(markOf(f.id).played));
		if (!f.description.empty())
			it->setToolTip(qs(f.description));
		if (f.id == current)
			currentRow = list_->count() - 1;
	}
	if (currentRow >= 0 && search_->text().isEmpty()) {
		list_->setCurrentRow(currentRow);
		list_->scrollToItem(list_->item(currentRow), QAbstractItemView::PositionAtCenter);
	} else if (keepRow >= 0 && keepRow < list_->count()) {
		list_->setCurrentRow(keepRow);
	} else if (list_->count()) {
		list_->setCurrentRow(0);
	}
	if (lib.empty())
		count_->setText(T("NoTabs"));
	else if (starredOnly_->isChecked() && rows_.empty())
		count_->setText(T("NoStarred"));
	else
		count_->setText(T("CountFormat").arg(rows_.size()).arg(lib.size()) + (starred ? T("StarredCount").arg(starred) : QString()));
}

// The starred tab you played most recently (never played: the last starred one in the list).
void TabPlayerDock::openLastFavorite()
{
	Engine &e = Engine::get();
	auto lib = e.library();
	auto marks = e.marks();
	const tab::LibraryItem *best = nullptr;
	int64_t bestPlayed = -1;
	for (auto &f : lib) {
		auto m = marks.find(f.id);
		if (m == marks.end() || !m->second.starred)
			continue;
		if (m->second.played >= bestPlayed) {
			best = &f;
			bestPlayed = m->second.played;
		}
	}
	if (best && best->id != e.settings().fileId)
		e.openFile(best->id);
}

void TabPlayerDock::filter()
{
	fillList();
}

void TabPlayerDock::refresh()
{
	Engine &e = Engine::get();
	Settings s = e.settings();
	auto lib = e.library();
	auto piece = e.piece();
	auto st = e.status();

	const bool hasLibrary = !lib.empty();
	const bool hasPiece = piece && !piece->fileId.empty() && piece->error.empty() && !piece->score.tracks.empty();
	welcome_->setVisible(s.library.empty() || lib.empty() || !sceneHasTabPlayer());
	playerBox_->setVisible(hasPiece);
	pages_->setVisible(hasLibrary || hasPiece);
	footer_->setVisible(!s.library.empty());
	if (starredOnly_->isChecked() != s.starredOnly) {
		QSignalBlocker block(starredOnly_);
		starredOnly_->setChecked(s.starredOnly);
		shownLibraryKey_.clear(); // list again with the remembered filter
	}
	if (hasLibrary && !pickedFavorite_) {
		pickedFavorite_ = true;
		if (!st.playing)
			openLastFavorite();
	}
	chooseBtn_->setText((s.library.empty() || lib.empty() ? QString::fromUtf8("1. ") : QString::fromUtf8("\xE2\x9C\x93 ")) + T("ChooseFolder"));
	addBtn_->setText((sceneHasTabPlayer() ? QString::fromUtf8("\xE2\x9C\x93 ") : QString::fromUtf8("2. ")) + T("AddToScene"));
	addBtn_->setEnabled(!sceneHasTabPlayer());

	// library list only when it changed
	std::string key = s.library + "|" + std::to_string(lib.size()) + "|" + s.fileId;
	size_t indexed = 0;
	for (auto &f : lib)
		indexed += f.indexed;
	key += "|" + std::to_string(indexed / 64);
	for (auto &[id, mark] : e.marks()) // stars and played days show in the list
		key += "|" + id + (mark.starred ? "*" : "") + std::to_string(mark.played / 3600);
	if (key != shownLibraryKey_) {
		shownLibraryKey_ = key;
		fillList();
	}
	libraryLabel_->setText(s.library.empty() ? T("NoFolder") : qs(s.library));
	libraryLabel_->setToolTip(qs(s.library));

	if (piece && !piece->fileId.empty()) {
		title_->setText(qs(tab::displayTitle(piece->item)));
		auto marks = e.marks();
		auto m = marks.find(piece->fileId);
		const bool on = m != marks.end() && m->second.starred;
		starBtn_->setText(QString::fromUtf8(on ? "\xE2\x98\x85" : "\xE2\x98\x86"));
		starBtn_->setStyleSheet(QString("font-size: 18px; padding: 0 4px; color: %1;").arg(on ? kStarColor.name() : "palette(text)"));
	} else {
		title_->setText(T("NothingLoaded"));
	}

	{
		QSignalBlocker b1(bpm_), b2(speed_), b3(loopBtn_), b4(countBtn_), b5(clickBtn_), b6(loopFrom_), b7(loopTo_), b8(theme_), b9(font_), b10(layout_), b11(autoSize_), b12(zoom_),
			b13(colors_), b14(notes_), b15(panel_), b16(rhythm_), b17(barNums_), b18(lit_), b19(tone_), b20(volume_), b21(clickVol_), b22(offset_), b23(track_), b24(floating_), b25(background_);
		floating_->setChecked(s.background <= 0.001 && !s.showPanel);
		background_->setValue(int(std::lround(s.background * 100)));
		double base = st.baseTempo;
		if (!bpm_->hasFocus())
			bpm_->setValue(base > 0 ? int(std::lround(base * s.speed)) : 0);
		bpm_->setEnabled(base > 0);
		speed_->setValue(int(std::lround(s.speed * 100)));
		speedLabel_->setText(QString::number(s.speed, 'f', 2) + QString::fromUtf8("\xC3\x97"));
		loopBtn_->setChecked(s.loop);
		countBtn_->setChecked(s.countIn);
		clickBtn_->setChecked(s.metronome);
		int bars = piece ? int(piece->score.masterBars.size()) : 0;
		loopFrom_->setMaximum(std::max(0, bars));
		loopTo_->setMaximum(std::max(0, bars));
		if (!loopFrom_->hasFocus())
			loopFrom_->setValue(s.loopFrom);
		if (!loopTo_->hasFocus())
			loopTo_->setValue(s.loopTo);
		theme_->setCurrentIndex(std::max(0, theme_->findData(qs(s.theme))));
		font_->setCurrentIndex(std::max(0, font_->findData(qs(s.font))));
		layout_->setCurrentIndex(s.horizontal ? 0 : 1);
		autoSize_->setChecked(s.zoom <= 0);
		zoom_->setEnabled(s.zoom > 0);
		zoom_->setValue(s.zoom > 0 ? int(s.zoom * 100) : 100);
		colors_->setChecked(s.colorStrings);
		notes_->setChecked(s.showNotes);
		panel_->setChecked(s.showPanel);
		rhythm_->setChecked(s.rhythm);
		barNums_->setChecked(s.barNumbers);
		lit_->setChecked(s.highlightNotes);
		tone_->setCurrentIndex(std::max(0, tone_->findData(qs(s.tone))));
		volume_->setValue(int(std::lround(s.volume * 100)));
		clickVol_->setValue(int(std::lround(s.clickVolume * 100)));
		offset_->setValue(s.avOffsetMs);
		offsetLabel_->setText(T("Offset").arg(s.avOffsetMs));
		track_->clear();
		if (piece)
			for (size_t i = 0; i < piece->score.tracks.size(); i++)
				track_->addItem(QString("%1. %2").arg(i + 1).arg(qs(piece->score.tracks[i].name)));
		track_->setCurrentIndex(s.track);
		track_->setEnabled(track_->count() > 1);

		bool range = piece && piece->notes.tempoMin > 0 && base > 0;
		rangeRow_->setVisible(range);
		if (range) {
			int lo = piece->notes.tempoMin, hi = piece->notes.tempoMax, mid = (lo + hi) / 2;
			int vals[3] = {lo, mid, hi};
			for (int i = 0; i < 3; i++) {
				rangeBtns_[i]->setText(QString::number(vals[i]));
				rangeBtns_[i]->setProperty("bpm", vals[i]);
			}
		}
	}
	shownVersion_ = e.version();
	refreshStatus();
}

void TabPlayerDock::refreshStatus()
{
	if (!isVisible())
		return;
	auto st = Engine::get().status();
	statusTimer_->setInterval(st.playing ? 200 : 1000); // only tick fast while something moves
	playBtn_->setIcon(QApplication::style()->standardIcon(st.playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
	playBtn_->setText(st.playing ? T("Pause") : T("Play"));
	auto mmss = [](double s) { int t = int(std::max(0.0, s)); return QString("%1:%2").arg(t / 60).arg(t % 60, 2, 10, QChar('0')); };
	if (st.bars > 0)
		info_->setText(st.countIn ? T("CountingIn").arg(st.countIn) : T("BarFormat").arg(std::max(1, st.bar)).arg(st.bars).arg(mmss(st.position)).arg(mmss(st.duration)));
	else
		info_->clear();
	progress_->setValue(st.duration > 0 ? int(1000 * st.position / st.duration) : 0);
	if (Engine::get().version() != shownVersion_)
		refresh();
}

bool TabPlayerDock::eventFilter(QObject *obj, QEvent *e)
{
	const auto type = e->type();
	if (type == QEvent::Wheel && (qobject_cast<QAbstractSlider *>(obj) || qobject_cast<QComboBox *>(obj) || qobject_cast<QAbstractSpinBox *>(obj))) {
		e->ignore(); // not for this control: the page under it scrolls instead
		return true;
	}
	if (list_ && obj == list_->viewport() &&
	    (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease || type == QEvent::MouseButtonDblClick)) {
		auto *me = static_cast<QMouseEvent *>(e);
		QPoint at = me->position().toPoint();
		auto *it = list_->itemAt(at);
		if (it && at.x() < kStarWidth + 4) { // the star: mark it, never open or play the tab
			if (type != QEvent::MouseButtonRelease && me->button() == Qt::LeftButton)
				Engine::get().toggleStar(su(it->data(IdRole).toString()));
			return true;
		}
	}
	if (obj == progress_ && e->type() == QEvent::MouseButtonPress) {
		auto *me = static_cast<QMouseEvent *>(e);
		double f = std::clamp(me->position().x() / std::max(1, progress_->width()), 0.0, 1.0);
		Engine::get().seek(f * Engine::get().status().duration);
		return true;
	}
	if ((obj == search_ || obj == list_) && e->type() == QEvent::KeyPress) {
		auto *ke = static_cast<QKeyEvent *>(e);
		if (obj == search_ && (ke->key() == Qt::Key_Down || ke->key() == Qt::Key_Up)) {
			int r = std::clamp(list_->currentRow() + (ke->key() == Qt::Key_Down ? 1 : -1), 0, std::max(0, list_->count() - 1));
			list_->setCurrentRow(r);
			return true;
		}
		if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
			openRow(list_->currentRow(), true);
			return true;
		}
		if (obj == search_ && ke->key() == Qt::Key_Escape) {
			search_->clear();
			return true;
		}
	}
	return QWidget::eventFilter(obj, e);
}

void TabPlayerDock::dragEnterEvent(QDragEnterEvent *e)
{
	if (e->mimeData()->hasUrls())
		e->acceptProposedAction();
}

void TabPlayerDock::dropEvent(QDropEvent *e)
{
	const auto urls = e->mimeData()->urls();
	for (const QUrl &u : urls)
		if (u.isLocalFile()) {
			Engine::get().openPath(su(QDir::toNativeSeparators(u.toLocalFile())));
			break;
		}
}

void createTabPlayerDock()
{
	auto *dock = new TabPlayerDock();
	g_dock = dock;
	if (!obs_frontend_add_dock_by_id("tab-player-dock", obs_module_text("TabPlayer"), dock)) {
		delete dock;
		g_dock = nullptr;
	}
}

void introduceTabPlayerDock()
{
	if (!g_dock)
		return;
	// Open the dock on the first start, and on any start while setup is unfinished.
	const bool setUp = !Engine::get().settings().library.empty() && !Engine::get().library().empty();
	if (Engine::get().settings().dockIntroduced && setUp)
		return;
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	QWidget *w = g_dock;
	while (w && !w->inherits("QDockWidget"))
		w = w->parentWidget();
	auto *dock = qobject_cast<QDockWidget *>(w);
	if (main && dock) {
		dock->setFloating(false);
		main->addDockWidget(Qt::RightDockWidgetArea, dock);
		dock->show();
		dock->raise();
	}
	Engine::get().update([](Settings &s) { s.dockIntroduced = true; });
}

void showTabPlayerDock()
{
	if (!g_dock)
		return;
	QWidget *w = g_dock;
	while (w && !w->inherits("QDockWidget"))
		w = w->parentWidget();
	if (w) {
		w->show();
		w->raise();
	}
}

} // namespace tabobs
