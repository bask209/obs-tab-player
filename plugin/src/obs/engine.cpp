// SPDX-License-Identifier: Apache-2.0
#include "engine.hpp"

#include "core/limits.hpp"

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace tabobs {
namespace fs = std::filesystem;

namespace {
constexpr int kRate = 48000;
constexpr int kBlock = 960; // 20 ms per block: 50 wake-ups per second while playing
constexpr uint64_t kBlockNs = 20'000'000;
constexpr uint64_t kLeadNs = 40'000'000; // audio is produced this far ahead

std::unique_ptr<Engine> g_engine;
std::mutex g_engineMu;

tab::Tone toneOf(const std::string &s) { return s == "warm" ? tab::Tone::Warm : s == "crunch" ? tab::Tone::Crunch : tab::Tone::Clean; }

bool readFile(const fs::path &p, std::vector<uint8_t> &out, std::string &error)
{
	std::error_code ec;
	auto size = fs::file_size(p, ec);
	if (ec) {
		error = "File not found";
		return false;
	}
	if (size > tab::limits::kMaxFileBytes) {
		error = "File is too large";
		return false;
	}
	std::ifstream in(p, std::ios::binary);
	if (!in) {
		error = "Cannot open the file";
		return false;
	}
	out.resize(size_t(size));
	if (size && !in.read(reinterpret_cast<char *>(out.data()), std::streamsize(size))) {
		error = "Cannot read the file";
		return false;
	}
	return true;
}
} // namespace

Engine &Engine::get()
{
	std::lock_guard<std::mutex> lock(g_engineMu);
	if (!g_engine)
		g_engine.reset(new Engine());
	return *g_engine;
}

void Engine::shutdown()
{
	std::unique_ptr<Engine> e;
	{
		std::lock_guard<std::mutex> lock(g_engineMu);
		e = std::move(g_engine);
	}
	if (e)
		e->stop();
}

void Engine::start(const std::string &configDir)
{
	if (running_.exchange(true))
		return;
	std::error_code ec;
	fs::create_directories(fs::u8path(configDir), ec);
	configFile_ = configDir + "/settings.json";
	load();
	{
		std::lock_guard<std::mutex> lock(mu_);
		library_ = tab::scanLibrary(settings_.library);
	}
	applySynthSettings();
	auto s = settings();
	if (!s.fileId.empty())
		loadPiece(s.fileId);
	audio_ = std::thread([this] { audioLoop(); });
	indexer_ = std::thread([this] { indexLoop(); });
}

void Engine::stop()
{
	if (!running_.exchange(false))
		return;
	wake_.notify_all();
	if (audio_.joinable())
		audio_.join();
	if (indexer_.joinable())
		indexer_.join();
	save();
}

// ------------------------------------------------------------------ settings

Settings Engine::settings() const
{
	std::lock_guard<std::mutex> lock(mu_);
	return settings_;
}

void Engine::update(const std::function<void(Settings &)> &change)
{
	Settings before, after;
	{
		std::lock_guard<std::mutex> lock(mu_);
		before = settings_;
		change(settings_);
		settings_.speed = std::clamp(settings_.speed, 0.1, 3.0);
		settings_.volume = std::clamp(settings_.volume, 0.0, 1.5);
		settings_.clickVolume = std::clamp(settings_.clickVolume, 0.0, 1.5);
		settings_.zoom = std::clamp(settings_.zoom, 0.0, 4.0);
		settings_.avOffsetMs = std::clamp(settings_.avOffsetMs, -500, 500);
		settings_.background = std::clamp(settings_.background, 0.0, 1.0);
		avOffsetMs_ = settings_.avOffsetMs;
		settings_.loopFrom = std::max(0, settings_.loopFrom);
		settings_.loopTo = std::max(0, settings_.loopTo);
		after = settings_;
	}
	if (after.library != before.library)
		rescan();
	if (after.fileId != before.fileId || after.track != before.track)
		loadPiece(after.fileId);
	applySynthSettings();
	version_++;
	save();
	notify();
}

void Engine::applySynthSettings()
{
	auto s = settings();
	synth_.setVolume(float(s.volume));
	synth_.setClickVolume(float(s.clickVolume));
	synth_.setMetronome(s.metronome);
	synth_.setSpeed(s.speed);
	synth_.setTone(toneOf(s.tone));
	auto p = piece();
	if (p && !p->timeline.bars.empty()) {
		auto [a, b] = tab::loopRange(p->timeline, s.loopFrom, s.loopTo);
		synth_.setLoop(s.loop, a, b);
	}
}

void Engine::load()
{
	obs_data_t *d = obs_data_create_from_json_file_safe(configFile_.c_str(), "bak");
	if (!d)
		return;
	std::lock_guard<std::mutex> lock(mu_);
	auto &s = settings_;
	auto str = [&](const char *k, std::string &v) { if (obs_data_has_user_value(d, k)) v = obs_data_get_string(d, k); };
	auto dbl = [&](const char *k, double &v) { if (obs_data_has_user_value(d, k)) v = obs_data_get_double(d, k); };
	auto bln = [&](const char *k, bool &v) { if (obs_data_has_user_value(d, k)) v = obs_data_get_bool(d, k); };
	auto num = [&](const char *k, int &v) { if (obs_data_has_user_value(d, k)) v = int(obs_data_get_int(d, k)); };
	str("library", s.library), str("fileId", s.fileId), num("track", s.track), dbl("speed", s.speed);
	bln("loop", s.loop), bln("countIn", s.countIn), bln("metronome", s.metronome), num("loopFrom", s.loopFrom), num("loopTo", s.loopTo);
	dbl("volume", s.volume), dbl("clickVolume", s.clickVolume), str("tone", s.tone), str("theme", s.theme), str("font", s.font);
	bln("colorStrings", s.colorStrings), bln("horizontal", s.horizontal), dbl("zoom", s.zoom), bln("rhythm", s.rhythm), bln("barNumbers", s.barNumbers);
	bln("highlightNotes", s.highlightNotes), bln("showPanel", s.showPanel), bln("showNotes", s.showNotes), num("avOffsetMs", s.avOffsetMs);
	dbl("background", s.background);
	avOffsetMs_ = s.avOffsetMs;
	bln("dockIntroduced", s.dockIntroduced);
	obs_data_release(d);
	s.speed = std::clamp(s.speed, 0.1, 3.0);
}

void Engine::save() const
{
	if (configFile_.empty())
		return;
	Settings s = settings();
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "library", s.library.c_str());
	obs_data_set_string(d, "fileId", s.fileId.c_str());
	obs_data_set_int(d, "track", s.track);
	obs_data_set_double(d, "speed", s.speed);
	obs_data_set_bool(d, "loop", s.loop);
	obs_data_set_bool(d, "countIn", s.countIn);
	obs_data_set_bool(d, "metronome", s.metronome);
	obs_data_set_int(d, "loopFrom", s.loopFrom);
	obs_data_set_int(d, "loopTo", s.loopTo);
	obs_data_set_double(d, "volume", s.volume);
	obs_data_set_double(d, "clickVolume", s.clickVolume);
	obs_data_set_string(d, "tone", s.tone.c_str());
	obs_data_set_string(d, "theme", s.theme.c_str());
	obs_data_set_string(d, "font", s.font.c_str());
	obs_data_set_bool(d, "colorStrings", s.colorStrings);
	obs_data_set_bool(d, "horizontal", s.horizontal);
	obs_data_set_double(d, "zoom", s.zoom);
	obs_data_set_bool(d, "rhythm", s.rhythm);
	obs_data_set_bool(d, "barNumbers", s.barNumbers);
	obs_data_set_bool(d, "highlightNotes", s.highlightNotes);
	obs_data_set_bool(d, "showPanel", s.showPanel);
	obs_data_set_bool(d, "showNotes", s.showNotes);
	obs_data_set_int(d, "avOffsetMs", s.avOffsetMs);
	obs_data_set_double(d, "background", s.background);
	obs_data_set_bool(d, "dockIntroduced", s.dockIntroduced);
	obs_data_save_json_safe(d, configFile_.c_str(), "tmp", "bak");
	obs_data_release(d);
}

// ------------------------------------------------------------------ library

std::vector<tab::LibraryItem> Engine::library() const
{
	std::lock_guard<std::mutex> lock(mu_);
	return library_;
}

void Engine::setLibrary(const std::string &folder)
{
	update([&](Settings &s) { s.library = folder; });
}

void Engine::rescan()
{
	std::string root;
	{
		std::lock_guard<std::mutex> lock(mu_);
		root = settings_.library;
	}
	auto items = tab::scanLibrary(root);
	{
		std::lock_guard<std::mutex> lock(mu_);
		library_ = std::move(items);
		bool found = std::any_of(library_.begin(), library_.end(), [&](auto &f) { return f.id == settings_.fileId; });
		if (!found && !library_.empty() && settings_.fileId.empty())
			settings_.fileId = library_.front().id;
	}
	libraryGen_++;
	wake_.notify_all();
	version_++;
	notify();
}

std::string Engine::absolutePath(const std::string &fileId) const
{
	std::lock_guard<std::mutex> lock(mu_);
	if (fs::u8path(fileId).is_absolute())
		return fileId;
	return settings_.library + "/" + fileId;
}

void Engine::openFile(const std::string &fileId)
{
	update([&](Settings &s) {
		if (s.fileId != fileId) {
			s.fileId = fileId;
			s.loopFrom = s.loopTo = 0;
			s.track = 0;
		}
	});
}

void Engine::openPath(const std::string &absolutePath)
{
	// A file dropped from anywhere: use its folder as the library and open it.
	fs::path p = fs::u8path(absolutePath);
	std::error_code ec;
	if (fs::is_directory(p, ec)) {
		setLibrary(p.u8string());
		return;
	}
	if (!tab::isTabFile(p.filename().u8string()))
		return;
	std::string folder = p.parent_path().u8string();
	std::string name = p.filename().u8string();
	bool inLibrary = false;
	{
		std::lock_guard<std::mutex> lock(mu_);
		std::string root = fs::u8path(settings_.library).lexically_normal().u8string();
		auto rel = fs::relative(p, fs::u8path(settings_.library), ec).u8string();
		inLibrary = !root.empty() && !ec && !rel.empty() && rel.rfind("..", 0) != 0;
		if (inLibrary) {
			std::replace(rel.begin(), rel.end(), '\\', '/');
			name = rel;
		}
	}
	if (!inLibrary)
		setLibrary(folder);
	openFile(name);
}

void Engine::indexLoop()
{
	uint64_t done = UINT64_MAX;
	while (running_) {
		uint64_t gen = libraryGen_.load();
		if (gen == done) {
			std::unique_lock<std::mutex> lock(wakeMu_);
			wake_.wait_for(lock, std::chrono::seconds(60)); // rescan() wakes it
			continue;
		}
		auto items = library();
		std::string root = settings().library;
		size_t changed = 0;
		for (size_t i = 0; i < items.size() && running_ && libraryGen_.load() == gen; i++) {
			if (items[i].indexed)
				continue;
			std::vector<uint8_t> bytes;
			std::string err;
			tab::LibraryItem meta = items[i];
			meta.indexed = true;
			if (readFile(fs::u8path(root + "/" + items[i].id), bytes, err)) {
				try {
					auto score = tab::loadScore(bytes.data(), bytes.size());
					auto notes = tab::parseNotes(score.notices, score.tempoLabel);
					meta.title = score.title + " " + score.subTitle + " " + score.artist;
					meta.description = notes.description;
					meta.tempoMin = notes.tempoMin;
					meta.tempoMax = notes.tempoMax;
				} catch (...) {
				}
			}
			{
				std::lock_guard<std::mutex> lock(mu_);
				if (libraryGen_.load() == gen && i < library_.size() && library_[i].id == meta.id)
					library_[i] = meta;
			}
			if (++changed % 64 == 0)
				notify();
			std::this_thread::sleep_for(std::chrono::milliseconds(1)); // stay in the background
		}
		if (libraryGen_.load() == gen) {
			done = gen;
			notify();
		}
	}
}

// ------------------------------------------------------------------ piece

void Engine::loadPiece(const std::string &fileId)
{
	auto p = std::make_shared<Piece>();
	p->fileId = fileId;
	p->item = tab::describeFile(fileId);
	if (!fileId.empty()) {
		std::vector<uint8_t> bytes;
		if (readFile(fs::u8path(absolutePath(fileId)), bytes, p->error)) {
			try {
				p->score = tab::loadScore(bytes.data(), bytes.size());
				int track = std::clamp(settings().track, 0, std::max(0, int(p->score.tracks.size()) - 1));
				// a band file opens on its first pitched track: drums have no tab to show
				if (track == 0 && !p->score.tracks.empty() && p->score.tracks[0].percussion)
					for (size_t t = 1; t < p->score.tracks.size(); t++)
						if (!p->score.tracks[t].percussion) {
							track = int(t);
							std::lock_guard<std::mutex> lock(mu_);
							settings_.track = track;
							break;
						}
				p->timeline = tab::buildTimeline(p->score, track);
				p->notes = tab::parseNotes(p->score.notices, p->score.tempoLabel);
			} catch (const std::exception &e) {
				p->error = e.what();
				p->score = tab::Score();
				p->timeline = tab::Timeline();
			}
		}
	}
	auto ci = tab::countInClicks(p->score, p->timeline, 0);
	synth_.load(p->timeline, &ci);
	{
		std::lock_guard<std::mutex> lock(mu_);
		piece_ = p;
	}
	applySynthSettings();
	version_++;
}

std::shared_ptr<const Piece> Engine::piece() const
{
	std::lock_guard<std::mutex> lock(mu_);
	return piece_;
}

Status Engine::status() const
{
	Status st;
	auto p = piece();
	st.playing = synth_.playing();
	st.duration = p ? p->timeline.duration : 0;
	st.position = positionAtTime(os_gettime_ns(), &st.countIn);
	if (p) {
		st.bars = int(p->score.masterBars.size());
		for (auto &b : p->timeline.bars)
			if (b.start <= st.position + 1e-6)
				st.bar = b.index + 1;
			else
				break;
		st.baseTempo = p->timeline.bars.empty() ? p->score.tempo : p->timeline.bars.front().bpm;
	}
	return st;
}

// ------------------------------------------------------------------ transport

void Engine::play()
{
	auto s = settings();
	auto p = piece();
	if (!p || p->timeline.notes.empty())
		return;
	auto ci = tab::countInClicks(p->score, p->timeline, synth_.positionAt(synth_.samplesRendered()));
	synth_.setCountIn(ci);
	synth_.play(synth_.positionAt(synth_.samplesRendered()), s.speed, s.countIn, s.loop);
	wake_.notify_all();
	notify();
}

void Engine::pause()
{
	synth_.pause();
	notify();
}

void Engine::playPause()
{
	if (synth_.playing())
		pause();
	else
		play();
}

void Engine::stopPlayback()
{
	synth_.stop();
	notify();
}

void Engine::restart()
{
	synth_.stop();
	play();
}

void Engine::seek(double seconds)
{
	synth_.seek(seconds);
	wake_.notify_all();
	notify();
}

void Engine::nextFile(int dir)
{
	auto lib = library();
	if (lib.empty())
		return;
	auto cur = settings().fileId;
	long i = 0;
	for (size_t k = 0; k < lib.size(); k++)
		if (lib[k].id == cur)
			i = long(k);
	long n = long(lib.size());
	openFile(lib[size_t(((i + dir) % n + n) % n)].id);
}

void Engine::nextDay(int dir)
{
	auto lib = library();
	std::vector<const tab::LibraryItem *> mains;
	for (auto &f : lib)
		if (f.day >= 0 && f.alt.empty())
			mains.push_back(&f);
	if (mains.empty())
		return nextFile(dir);
	int curDay = tab::describeFile(settings().fileId).day;
	const tab::LibraryItem *target = nullptr;
	if (dir > 0) {
		for (auto *f : mains)
			if (f->day > curDay) { target = f; break; }
		if (!target) target = mains.front();
	} else {
		for (auto it = mains.rbegin(); it != mains.rend(); ++it)
			if ((*it)->day < curDay) { target = *it; break; }
		if (!target) target = mains.back();
	}
	openFile(target->id);
}

void Engine::speedStep(double delta)
{
	update([&](Settings &s) { s.speed = std::round((s.speed + delta) * 100) / 100; });
}

void Engine::setBpm(double bpm)
{
	auto p = piece();
	double base = p && !p->timeline.bars.empty() ? p->timeline.bars.front().bpm : 0;
	if (base > 0 && bpm > 0)
		update([&](Settings &s) { s.speed = std::round(bpm / base * 1000) / 1000; });
}

// ------------------------------------------------------------------ audio + clock

double Engine::positionAtTime(uint64_t ns, int *countInBeat) const
{
	int64_t sample;
	{
		std::lock_guard<std::mutex> lock(clockMu_);
		if (clockValid_)
			sample = clockSample_ + int64_t((int64_t(ns) - int64_t(clockNs_)) * kRate / 1'000'000'000);
		else
			sample = synth_.samplesRendered();
	}
	sample -= int64_t(avOffsetMs_.load(std::memory_order_relaxed)) * kRate / 1000;
	return synth_.positionAndCountIn(sample, countInBeat);
}

void Engine::registerSource(obs_source_t *source, bool metronome)
{
	std::lock_guard<std::mutex> lock(sourcesMu_);
	auto &list = metronome ? clickSources_ : sources_;
	if (std::find(list.begin(), list.end(), source) == list.end())
		list.push_back(source);
}

void Engine::unregisterSource(obs_source_t *source)
{
	std::lock_guard<std::mutex> lock(sourcesMu_);
	sources_.erase(std::remove(sources_.begin(), sources_.end(), source), sources_.end());
	clickSources_.erase(std::remove(clickSources_.begin(), clickSources_.end(), source), clickSources_.end());
}

void Engine::audioLoop()
{
	os_set_thread_name("tab-player-audio");
	std::vector<float> buf(kBlock), clicks(kBlock);
	uint64_t next = 0;
	bool active = false;
	uint64_t audioNs = 0, audioBlocks = 0, lastLog = os_gettime_ns();
	auto logPerf = [&] {
		uint64_t frames = renderFrames_.exchange(0), rns = renderNs_.exchange(0), rmax = renderMax_.exchange(0);
		if (audioBlocks || frames)
			blog(LOG_INFO, "[tab-player] perf: video %.1f us/frame (max %.1f, %llu frames), audio %.1f us per %d ms block (%.3f%% of one core)",
			     frames ? rns / 1000.0 / double(frames) : 0.0, rmax / 1000.0, (unsigned long long)frames, audioBlocks ? audioNs / 1000.0 / double(audioBlocks) : 0.0,
			     int(kBlockNs / 1'000'000), audioBlocks ? 100.0 * double(audioNs) / double(audioBlocks * kBlockNs) : 0.0);
		audioNs = audioBlocks = 0;
	};
	while (running_) {
		if (synth_.idle()) {
			if (active) {
				std::lock_guard<std::mutex> lock(clockMu_);
				clockValid_ = false;
				active = false;
			}
			if (audioBlocks)
				logPerf();
			std::unique_lock<std::mutex> lock(wakeMu_);
			wake_.wait_for(lock, std::chrono::seconds(1)); // play() and seek() wake it
			continue;
		}
		uint64_t now = os_gettime_ns();
		if (!active || now > next + 250'000'000ull) { // (re)start the clock
			next = now;
			active = true;
		}
		// produce everything due within the lead time, then sleep until the next block is due
		while (running_ && next <= now + kLeadNs) {
			int64_t sample = synth_.samplesRendered();
			{
				std::lock_guard<std::mutex> lock(clockMu_);
				clockSample_ = sample;
				clockNs_ = next;
				clockValid_ = true;
			}
			bool split;
			{
				std::lock_guard<std::mutex> lock(sourcesMu_);
				split = !clickSources_.empty();
			}
			uint64_t t0 = os_gettime_ns();
			// clicks get their own source (own volume and tracks) when it exists, else they mix in
			synth_.render(buf.data(), buf.size(), split ? clicks.data() : nullptr);
			audioNs += os_gettime_ns() - t0;
			audioBlocks++;
			obs_source_audio audio = {};
			audio.data[0] = reinterpret_cast<const uint8_t *>(buf.data());
			audio.data[1] = reinterpret_cast<const uint8_t *>(buf.data());
			audio.frames = kBlock;
			audio.speakers = SPEAKERS_STEREO;
			audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
			audio.samples_per_sec = kRate;
			audio.timestamp = next;
			{
				std::lock_guard<std::mutex> lock(sourcesMu_);
				if (!sources_.empty())
					obs_source_output_audio(sources_.front(), &audio);
				if (split && !clickSources_.empty()) {
					audio.data[0] = audio.data[1] = reinterpret_cast<const uint8_t *>(clicks.data());
					obs_source_output_audio(clickSources_.front(), &audio);
				}
			}
			next += kBlockNs;
		}
		if (now - lastLog > 10'000'000'000ull) {
			logPerf();
			lastLog = now;
		}
		uint64_t after = os_gettime_ns();
		uint64_t sleepNs = next > after + kLeadNs ? next - after - kLeadNs : 1'000'000;
		std::unique_lock<std::mutex> lock(wakeMu_);
		wake_.wait_for(lock, std::chrono::nanoseconds(std::min<uint64_t>(sleepNs, kBlockNs)));
	}
}

// ------------------------------------------------------------------ listeners

int Engine::listen(Listener fn)
{
	std::lock_guard<std::mutex> lock(listenMu_);
	int id = nextListener_++;
	listeners_.emplace_back(id, std::move(fn));
	return id;
}

void Engine::unlisten(int id)
{
	std::lock_guard<std::mutex> lock(listenMu_);
	listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(), [&](auto &l) { return l.first == id; }), listeners_.end());
}

void Engine::notify()
{
	std::vector<Listener> copy;
	{
		std::lock_guard<std::mutex> lock(listenMu_);
		for (auto &l : listeners_)
			copy.push_back(l.second);
	}
	for (auto &fn : copy)
		fn();
}

} // namespace tabobs
