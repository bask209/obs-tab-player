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
#include <QListWidget>
#include <QMainWindow>
#include <QDockWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
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

enum Roles { DayRole = Qt::UserRole + 1, AltRole, TopicRole, RangeRole, IdRole, CurrentRole };

// List rows: day number, version chip, topic, tempo range.
class RowDelegate : public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;
	QSize sizeHint(const QStyleOptionViewItem &o, const QModelIndex &) const override
	{
		return QSize(200, o.fontMetrics.height() + 10);
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
		int rw = range.isEmpty() ? 0 : QFontMetrics(f).horizontalAdvance(range) + 8;
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

QPushButton *toggle(const QString &text, const QString &tip)
{
	auto *b = new QPushButton(text);
	b->setCheckable(true);
	b->setToolTip(tip);
	b->setCursor(Qt::PointingHandCursor);
	return b;
}

QFrame *section(const QString &title, QWidget *content, bool open, QWidget *parent)
{
	auto *frame = new QFrame(parent);
	auto *lay = new QVBoxLayout(frame);
	lay->setContentsMargins(0, 0, 0, 0);
	lay->setSpacing(4);
	auto *head = new QToolButton(frame);
	head->setText(title);
	head->setCheckable(true);
	head->setChecked(open);
	head->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	head->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
	head->setAutoRaise(true);
	head->setStyleSheet("QToolButton { font-weight: bold; }");
	content->setVisible(open);
	QObject::connect(head, &QToolButton::toggled, frame, [head, content](bool on) {
		head->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
		content->setVisible(on);
	});
	lay->addWidget(head);
	lay->addWidget(content);
	return frame;
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

void TabPlayerDock::addTabPlayerToScene()
{
	obs_source_t *sceneSource = obs_frontend_get_current_scene();
	if (!sceneSource)
		return;
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	// reuse an existing Tab Player source if there is one, otherwise create it
	obs_source_t *src = nullptr;
	obs_enum_sources(
		[](void *param, obs_source_t *s) {
			if (strcmp(obs_source_get_unversioned_id(s), kTabSourceId) == 0) {
				*static_cast<obs_source_t **>(param) = obs_source_get_ref(s);
				return false;
			}
			return true;
		},
		&src);
	if (!src)
		src = obs_source_create(kTabSourceId, obs_module_text("TabPlayer"), nullptr, nullptr);
	if (src) {
		obs_sceneitem_t *item = obs_scene_add(scene, src);
		obs_video_info ovi = {};
		if (item && obs_get_video_info(&ovi)) {
			// half the canvas width, bottom-right: next to a camera on the left
			float w = float(obs_source_get_width(src)), h = float(obs_source_get_height(src));
			float scale = w > 0 ? (ovi.base_width * 0.5f) / w : 1.f;
			vec2 s = {scale, scale}, pos = {ovi.base_width - w * scale, ovi.base_height - h * scale};
			obs_sceneitem_set_scale(item, &s);
			obs_sceneitem_set_pos(item, &pos);
		}
		obs_source_release(src);
	}
	obs_source_release(sceneSource);
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
	Engine &e = Engine::get();
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	outer->addWidget(scroll);
	auto *body = new QWidget(scroll);
	scroll->setWidget(body);
	auto *v = new QVBoxLayout(body);
	v->setContentsMargins(8, 8, 8, 8);
	v->setSpacing(8);

	// --- welcome (only until set up)
	welcome_ = new QFrame(body);
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

	// Everything below appears only when it is useful: the player once a tab is loaded,
	// the list once a folder is chosen. A first-time user sees just the two set-up steps.
	playerBox_ = new QWidget(body);
	auto *pv = new QVBoxLayout(playerBox_);
	pv->setContentsMargins(0, 0, 0, 0);
	pv->setSpacing(8);
	v->addWidget(playerBox_);
	libraryBox_ = new QWidget(body);
	auto *lv = new QVBoxLayout(libraryBox_);
	lv->setContentsMargins(0, 0, 0, 0);
	lv->setSpacing(6);

	// --- now playing
	title_ = new QLabel(body);
	title_->setStyleSheet("font-weight: bold; font-size: 15px;");
	title_->setWordWrap(true);
	info_ = new QLabel(body);
	info_->setStyleSheet("color: palette(mid);");
	progress_ = new QProgressBar(body);
	progress_->setTextVisible(false);
	progress_->setMaximumHeight(6);
	progress_->setRange(0, 1000);
	progress_->setCursor(Qt::PointingHandCursor);
	progress_->setToolTip(T("SeekTip"));
	progress_->installEventFilter(this);
	upNext_ = new QPushButton(body);
	upNext_->setCursor(Qt::PointingHandCursor);
	upNext_->setFlat(true);
	upNext_->setStyleSheet("text-align: left;");
	connect(upNext_, &QPushButton::clicked, this, [] { Engine::get().nextDay(1); });
	pv->addWidget(title_);
	pv->addWidget(info_);
	pv->addWidget(progress_);

	// --- transport
	auto *tr = new QHBoxLayout();
	auto *style = QApplication::style();
	auto *prev = new QPushButton(style->standardIcon(QStyle::SP_MediaSkipBackward), QString(), body);
	playBtn_ = new QPushButton(body);
	auto *stopBtn = new QPushButton(style->standardIcon(QStyle::SP_MediaStop), QString(), body);
	auto *next = new QPushButton(style->standardIcon(QStyle::SP_MediaSkipForward), QString(), body);
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
	pv->addWidget(upNext_);
	connect(prev, &QPushButton::clicked, this, [] { Engine::get().nextDay(-1); });
	connect(next, &QPushButton::clicked, this, [] { Engine::get().nextDay(1); });
	connect(playBtn_, &QPushButton::clicked, this, [] { Engine::get().playPause(); });
	connect(stopBtn, &QPushButton::clicked, this, [] { Engine::get().stopPlayback(); });

	// --- tempo
	auto *tempoBox = new QFrame(body);
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
	auto *chips = new QHBoxLayout();
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
	rr->addWidget(rl);
	for (auto &b : rangeBtns_) {
		b = new QPushButton(rangeRow_);
		b->setCursor(Qt::PointingHandCursor);
		rr->addWidget(b);
		connect(b, &QPushButton::clicked, this, [b] { Engine::get().setBpm(b->property("bpm").toDouble()); });
	}
	tv->addWidget(rangeRow_);
	pv->addWidget(tempoBox);
	connect(slower, &QPushButton::clicked, this, [] { Engine::get().speedStep(-0.05); });
	connect(faster, &QPushButton::clicked, this, [] { Engine::get().speedStep(0.05); });
	connect(bpm_, qOverload<int>(&QSpinBox::valueChanged), this, [](int bpm) { Engine::get().setBpm(bpm); });
	connect(speed_, &QSlider::valueChanged, this, [](int v) { Engine::get().update([v](Settings &s) { s.speed = v / 100.0; }); });

	// --- practice toggles
	auto *pr = new QHBoxLayout();
	loopBtn_ = toggle(T("Loop"), T("LoopTip"));
	countBtn_ = toggle(T("CountIn"), T("CountInTip"));
	clickBtn_ = toggle(T("Metronome"), T("MetronomeTip"));
	for (auto *b : {loopBtn_, countBtn_, clickBtn_})
		pr->addWidget(b);
	pv->addLayout(pr);
	connect(loopBtn_, &QPushButton::toggled, this, [](bool on) { Engine::get().update([on](Settings &s) { s.loop = on; }); });
	connect(countBtn_, &QPushButton::toggled, this, [](bool on) { Engine::get().update([on](Settings &s) { s.countIn = on; }); });
	connect(clickBtn_, &QPushButton::toggled, this, [](bool on) { Engine::get().update([on](Settings &s) { s.metronome = on; }); });
	auto *lr = new QHBoxLayout();
	lr->addWidget(new QLabel(T("LoopBars"), body));
	loopFrom_ = new QSpinBox(body);
	loopTo_ = new QSpinBox(body);
	for (auto *sb : {loopFrom_, loopTo_}) {
		sb->setRange(0, 9999);
		sb->setSpecialValueText(T("All"));
		sb->setKeyboardTracking(false);
	}
	lr->addWidget(loopFrom_);
	lr->addWidget(new QLabel(QString::fromUtf8("\xE2\x80\x93"), body));
	lr->addWidget(loopTo_);
	lr->addStretch();
	pv->addLayout(lr);
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

	// --- find a tab
	search_ = new QLineEdit(body);
	search_->setPlaceholderText(T("SearchPlaceholder"));
	search_->setClearButtonEnabled(true);
	search_->installEventFilter(this);
	count_ = new QLabel(body);
	count_->setStyleSheet("color: palette(mid);");
	list_ = new QListWidget(body);
	list_->setItemDelegate(new RowDelegate(list_));
	list_->setUniformItemSizes(true);
	list_->setMinimumHeight(220);
	list_->installEventFilter(this);
	lv->addWidget(search_);
	lv->addWidget(count_);
	lv->addWidget(list_, 1);
	v->addWidget(libraryBox_, 1);
	connect(search_, &QLineEdit::textChanged, this, &TabPlayerDock::filter);
	connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) { openRow(list_->row(it), false); });
	connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem *it) { openRow(list_->row(it), true); });

	// --- look
	auto *look = new QWidget(body);
	auto *lg = new QGridLayout(look);
	lg->setContentsMargins(0, 0, 0, 0);
	theme_ = new QComboBox(look);
	for (auto &[id, key] : std::vector<std::pair<const char *, const char *>>{{"dark", "ThemeDark"}, {"light", "ThemeLight"}, {"glass", "ThemeGlass"}, {"clear", "ThemeClear"}, {"neon", "ThemeNeon"}, {"paper", "ThemePaper"}})
		theme_->addItem(T(key), id);
	font_ = new QComboBox(look);
	for (auto &[id, key] : std::vector<std::pair<const char *, const char *>>{{"sans", "FontSans"}, {"rounded", "FontRounded"}, {"mono", "FontMono"}, {"serif", "FontSerif"}})
		font_->addItem(T(key), id);
	layout_ = new QComboBox(look);
	layout_->addItem(T("LayoutRows"), false);
	layout_->addItem(T("LayoutLine"), true);
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
	int row = 0;
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
	lg->addWidget(colors_, row, 0);
	lg->addWidget(lit_, row++, 1);
	lg->addWidget(panel_, row, 0);
	lg->addWidget(notes_, row++, 1);
	lg->addWidget(rhythm_, row, 0);
	lg->addWidget(barNums_, row++, 1);
	pv->addWidget(section(T("Look"), look, false, body));
	auto setStr = [](std::string Settings::*field) { return [field](const QString &id) { Engine::get().update([&](Settings &s) { s.*field = su(id); }); }; };
	connect(theme_, &QComboBox::currentIndexChanged, this, [this, setStr](int) { setStr(&Settings::theme)(theme_->currentData().toString()); });
	connect(font_, &QComboBox::currentIndexChanged, this, [this, setStr](int) { setStr(&Settings::font)(font_->currentData().toString()); });
	connect(layout_, &QComboBox::currentIndexChanged, this, [this](int) { bool h = layout_->currentData().toBool(); Engine::get().update([h](Settings &s) { s.horizontal = h; }); });
	connect(track_, &QComboBox::currentIndexChanged, this, [](int i) { if (i >= 0) Engine::get().update([i](Settings &s) { s.track = i; }); });
	connect(autoSize_, &QCheckBox::toggled, this, [this](bool on) { double z = on ? 0 : zoom_->value() / 100.0; Engine::get().update([z](Settings &s) { s.zoom = z; }); });
	connect(zoom_, &QSlider::valueChanged, this, [this](int v) { if (!autoSize_->isChecked()) Engine::get().update([v](Settings &s) { s.zoom = v / 100.0; }); });
	auto setBool = [this](QCheckBox *box, bool Settings::*field) { connect(box, &QCheckBox::toggled, this, [field](bool on) { Engine::get().update([&](Settings &s) { s.*field = on; }); }); };
	setBool(colors_, &Settings::colorStrings);
	setBool(notes_, &Settings::showNotes);
	setBool(panel_, &Settings::showPanel);
	setBool(rhythm_, &Settings::rhythm);
	setBool(barNums_, &Settings::barNumbers);
	setBool(lit_, &Settings::highlightNotes);

	// --- sound
	auto *sound = new QWidget(body);
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
	row = 0;
	sg->addWidget(new QLabel(T("Tone"), sound), row, 0);
	sg->addWidget(tone_, row++, 1);
	sg->addWidget(new QLabel(T("GuitarVolume"), sound), row, 0);
	sg->addWidget(volume_, row++, 1);
	sg->addWidget(new QLabel(T("ClickVolume"), sound), row, 0);
	sg->addWidget(clickVol_, row++, 1);
	sg->addWidget(offsetLabel_, row, 0);
	sg->addWidget(offset_, row++, 1);
	pv->addWidget(section(T("Sound"), sound, false, body));
	connect(tone_, &QComboBox::currentIndexChanged, this, [this, setStr](int) { setStr(&Settings::tone)(tone_->currentData().toString()); });
	connect(volume_, &QSlider::valueChanged, this, [](int x) { Engine::get().update([x](Settings &s) { s.volume = x / 100.0; }); });
	connect(clickVol_, &QSlider::valueChanged, this, [](int x) { Engine::get().update([x](Settings &s) { s.clickVolume = x / 100.0; }); });
	connect(offset_, &QSlider::valueChanged, this, [](int x) { Engine::get().update([x](Settings &s) { s.avOffsetMs = x; }); });

	// --- library footer
	footer_ = new QWidget(body);
	auto *lib = new QHBoxLayout(footer_);
	lib->setContentsMargins(0, 0, 0, 0);
	libraryLabel_ = new QLabel(body);
	libraryLabel_->setStyleSheet("color: palette(mid);");
	libraryLabel_->setWordWrap(true);
	auto *change = new QPushButton(T("Change"), body);
	auto *rescan = new QPushButton(T("Rescan"), body);
	lib->addWidget(libraryLabel_, 1);
	lib->addWidget(change);
	lib->addWidget(rescan);
	v->addWidget(footer_);
	v->addStretch(0);
	welcome_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
	playerBox_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
	connect(change, &QPushButton::clicked, this, &TabPlayerDock::chooseFolder);
	connect(rescan, &QPushButton::clicked, this, [] { Engine::get().rescan(); });
	(void)e;
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
	rows_ = tab::searchLibrary(lib, su(search_->text()));
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
	count_->setText(lib.empty() ? T("NoTabs") : T("CountFormat").arg(rows_.size()).arg(lib.size()));
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
	libraryBox_->setVisible(hasLibrary);
	footer_->setVisible(!s.library.empty());
	chooseBtn_->setText((s.library.empty() || lib.empty() ? QString::fromUtf8("1. ") : QString::fromUtf8("\xE2\x9C\x93 ")) + T("ChooseFolder"));
	addBtn_->setText((sceneHasTabPlayer() ? QString::fromUtf8("\xE2\x9C\x93 ") : QString::fromUtf8("2. ")) + T("AddToScene"));
	addBtn_->setEnabled(!sceneHasTabPlayer());

	// library list only when it changed
	std::string key = s.library + "|" + std::to_string(lib.size()) + "|" + s.fileId;
	size_t indexed = 0;
	for (auto &f : lib)
		indexed += f.indexed;
	key += "|" + std::to_string(indexed / 64);
	if (key != shownLibraryKey_) {
		shownLibraryKey_ = key;
		fillList();
	}
	libraryLabel_->setText(s.library.empty() ? T("NoFolder") : qs(s.library));

	if (piece && !piece->fileId.empty()) {
		title_->setText(qs(tab::displayTitle(piece->item)));
		std::string next;
		for (auto &f : lib)
			if (f.day > piece->item.day && f.alt.empty() && piece->item.day >= 0) {
				next = tab::displayTitle(f);
				break;
			}
		upNext_->setVisible(!next.empty());
		upNext_->setText(T("UpNext").arg(qs(next)));
	} else {
		title_->setText(T("NothingLoaded"));
		upNext_->setVisible(false);
	}

	{
		QSignalBlocker b1(bpm_), b2(speed_), b3(loopBtn_), b4(countBtn_), b5(clickBtn_), b6(loopFrom_), b7(loopTo_), b8(theme_), b9(font_), b10(layout_), b11(autoSize_), b12(zoom_),
			b13(colors_), b14(notes_), b15(panel_), b16(rhythm_), b17(barNums_), b18(lit_), b19(tone_), b20(volume_), b21(clickVol_), b22(offset_), b23(track_);
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
		layout_->setCurrentIndex(s.horizontal ? 1 : 0);
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
