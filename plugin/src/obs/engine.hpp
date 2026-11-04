// SPDX-License-Identifier: Apache-2.0
// The player: settings, library, the loaded tab, the synth and its audio thread.
// One engine per OBS instance; sources and the dock observe it.
#pragma once

#include "core/library.hpp"
#include "core/synth.hpp"
#include "core/timeline.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct obs_source;
typedef struct obs_source obs_source_t;

namespace tabobs {

struct Settings {
	std::string library;
	std::string fileId;
	int track = 0;
	double speed = 1;
	bool loop = true, countIn = true, metronome = false;
	int loopFrom = 0, loopTo = 0;
	double volume = 0.9, clickVolume = 0.8;
	std::string tone = "clean";
	std::string theme = "dark";
	std::string font = "sans";
	bool colorStrings = false;
	bool horizontal = true; // one scrolling line (rows wrap like a page)
	double zoom = 0; // 0 = fit to the source automatically
	bool rhythm = true, barNumbers = true, highlightNotes = true;
	bool showPanel = true, showNotes = true;
	int avOffsetMs = 0;
	double background = 1; // card and panel opacity, 0 = only the floating tab
	bool dockIntroduced = false; // the dock was opened for the user once
	bool starredOnly = false; // the dock lists only the starred tabs
	int loopGap = 1; // between repeats: 0 straight on, 1 one silent bar, 2 a count-in bar
};

// What you marked in a tabs folder: a star, and when you last played the tab (Unix seconds).
struct Mark {
	bool starred = false;
	int64_t played = 0;
};

// Snapshot of what is loaded, shared read-only with sources (swapped atomically).
struct Piece {
	std::string fileId;
	tab::LibraryItem item;
	tab::Score score;
	tab::Timeline timeline;
	tab::NotesInfo notes;
	std::string error;
};

struct Status {
	bool playing = false;
	double position = 0, duration = 0;
	int bar = 0, bars = 0;
	int countIn = 0;
	double baseTempo = 0;
};

class Engine {
public:
	static Engine &get();
	static void shutdown();

	void start(const std::string &configDir);
	void stop();

	// ---- settings (UI thread) ----
	Settings settings() const;
	void update(const std::function<void(Settings &)> &change);
	uint64_t version() const { return version_.load(); } // bumps on every visible change

	// ---- library ----
	std::vector<tab::LibraryItem> library() const;
	void setLibrary(const std::string &folder);
	void rescan();
	void openFile(const std::string &fileId);
	void openPath(const std::string &absolutePath); // dropped onto the dock
	// Stars and last-played times for the current folder, by file id. Each folder keeps its own.
	std::map<std::string, Mark> marks() const;
	void toggleStar(const std::string &fileId); // empty: the tab that is open

	// ---- transport ----
	void play();
	void pause();
	void playPause();
	void stopPlayback();
	void restart();
	void seek(double seconds);
	void nextDay(int dir);
	void nextFile(int dir);
	void speedStep(double delta);
	// The bar playing now becomes the first / the last bar of the loop.
	void loopFromHere();
	void loopToHere();
	void setBpm(double bpm);

	std::shared_ptr<const Piece> piece() const;
	Status status() const;
	bool isPlaying() const { return synth_.playing(); }

	// ---- for sources ----
	// Song position and count-in beat at an OBS frame time (ns).
	double positionAtTime(uint64_t ns, int *countInBeat = nullptr) const;
	// metronome = the "Tab Player Metronome" source: clicks go there, on their own mixer channel.
	void registerSource(obs_source_t *source, bool metronome = false);
	void unregisterSource(obs_source_t *source);

	// UI observers (called on the thread that changed state; the dock re-posts to its thread).
	// Lightweight timing, logged every 10 s while playing (see audioLoop).
	void addRenderTime(uint64_t ns)
	{
		renderNs_ += ns;
		renderFrames_++;
		uint64_t m = renderMax_.load();
		while (ns > m && !renderMax_.compare_exchange_weak(m, ns)) {
		}
	}

	using Listener = std::function<void()>;
	int listen(Listener fn);
	void unlisten(int id);

private:
	Engine() = default;
	void notify();
	void save() const;
	void load();
	void loadMarks();
	void saveMarks() const;
	void applySynthSettings();
	void loadPiece(const std::string &fileId);
	void audioLoop();
	void indexLoop();
	std::string absolutePath(const std::string &fileId) const;

	mutable std::mutex mu_;
	Settings settings_;
	std::string configFile_, marksFile_;
	std::map<std::string, std::map<std::string, Mark>> marks_; // folder -> file id -> mark
	std::vector<tab::LibraryItem> library_;
	std::shared_ptr<const Piece> piece_;
	std::atomic<uint64_t> version_{1};

	tab::Synth synth_{48000};
	std::thread audio_, indexer_;
	std::atomic<bool> running_{false};
	std::condition_variable wake_;
	std::mutex wakeMu_;
	std::atomic<uint64_t> libraryGen_{0};
	std::atomic<uint64_t> renderNs_{0}, renderFrames_{0}, renderMax_{0};
	std::atomic<int> avOffsetMs_{0}; // read every video frame without copying Settings

	// clock mapping written by the audio thread
	mutable std::mutex clockMu_;
	int64_t clockSample_ = 0;
	uint64_t clockNs_ = 0;
	bool clockValid_ = false;

	mutable std::mutex sourcesMu_;
	std::vector<obs_source_t *> sources_;      // weak: registered/unregistered by the sources
	std::vector<obs_source_t *> clickSources_; // weak, likewise

	std::mutex listenMu_;
	std::vector<std::pair<int, Listener>> listeners_;
	int nextListener_ = 1;
};

} // namespace tabobs
