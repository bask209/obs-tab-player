// SPDX-License-Identifier: Apache-2.0
// The Tab Player dock: pick tabs, play, tempo, practice options, look and sound.
#pragma once

#include <QPointer>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QComboBox;
class QFrame;
class QTimer;
class QProgressBar;
class QToolButton;
class QTabWidget;

namespace tabobs {

class TabPlayerDock : public QWidget {
	Q_OBJECT
public:
	explicit TabPlayerDock(QWidget *parent = nullptr);
	~TabPlayerDock() override;

	static bool sceneHasTabPlayer();
	static void addTabPlayerToScene();
	static void ensureMetronomeSource();

protected:
	void dragEnterEvent(class QDragEnterEvent *e) override;
	void dropEvent(class QDropEvent *e) override;
	bool eventFilter(QObject *obj, QEvent *e) override;

private slots:
	void refresh();
	void refreshStatus();
	void filter();

private:
	void buildUi();
	void chooseFolder();
	void openRow(int row, bool play);
	void fillList();
	void openLastFavorite();

	int listener_ = 0;
	QTimer *statusTimer_ = nullptr;
	uint64_t shownVersion_ = 0;
	std::string shownLibraryKey_;
	std::vector<size_t> rows_;

	// welcome
	QFrame *welcome_ = nullptr;
	QPushButton *chooseBtn_ = nullptr, *addBtn_ = nullptr;
	QWidget *playerBox_ = nullptr, *footer_ = nullptr;
	QTabWidget *pages_ = nullptr;
	bool pickedFavorite_ = false; // opened the last starred tab once this session
	// now playing
	QLabel *title_ = nullptr, *info_ = nullptr;
	QPushButton *starBtn_ = nullptr;
	QProgressBar *progress_ = nullptr;
	// transport
	QPushButton *playBtn_ = nullptr;
	// tempo
	QSpinBox *bpm_ = nullptr;
	QSlider *speed_ = nullptr;
	QLabel *speedLabel_ = nullptr;
	QWidget *rangeRow_ = nullptr;
	QPushButton *rangeBtns_[3] = {};
	// practice
	QPushButton *loopBtn_ = nullptr, *countBtn_ = nullptr, *clickBtn_ = nullptr;
	QSpinBox *loopFrom_ = nullptr, *loopTo_ = nullptr;
	QPushButton *loopFromBtn_ = nullptr, *loopToBtn_ = nullptr;
	QComboBox *loopGap_ = nullptr;
	// search
	QLineEdit *search_ = nullptr;
	QPushButton *starredOnly_ = nullptr;
	QListWidget *list_ = nullptr;
	QLabel *count_ = nullptr;
	// look + sound
	QComboBox *theme_ = nullptr, *font_ = nullptr, *layout_ = nullptr, *tone_ = nullptr, *track_ = nullptr;
	QCheckBox *autoSize_ = nullptr, *colors_ = nullptr, *notes_ = nullptr, *panel_ = nullptr, *rhythm_ = nullptr, *barNums_ = nullptr, *lit_ = nullptr;
	QSlider *zoom_ = nullptr, *volume_ = nullptr, *clickVol_ = nullptr, *offset_ = nullptr, *background_ = nullptr;
	QCheckBox *floating_ = nullptr;
	QLabel *offsetLabel_ = nullptr, *libraryLabel_ = nullptr;
};

void createTabPlayerDock();
// First launch only: OBS adds plugin docks hidden, so open ours on the right once.
void introduceTabPlayerDock();

} // namespace tabobs
