// SPDX-License-Identifier: Apache-2.0
// OBS Tab Player: native OBS plugin entry point.
#include "obs/engine.hpp"
#include "obs/tab_source.hpp"
#include "ui/dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <functional>
#include <string>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-tab-player", "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "OBS Tab Player";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Scrolling Guitar Pro tabs with synced audio, as a native OBS source with a control dock.";
}

namespace {

struct Hotkey {
	const char *name;
	const char *textKey;
	std::function<void()> action;
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
};

std::vector<Hotkey> &hotkeys()
{
	using tabobs::Engine;
	static std::vector<Hotkey> list = {
		{"tab_player.play_pause", "HotkeyPlayPause", [] { Engine::get().playPause(); }},
		{"tab_player.stop", "HotkeyStop", [] { Engine::get().stopPlayback(); }},
		{"tab_player.restart", "HotkeyRestart", [] { Engine::get().restart(); }},
		{"tab_player.next_day", "HotkeyNextDay", [] { Engine::get().nextDay(1); }},
		{"tab_player.prev_day", "HotkeyPrevDay", [] { Engine::get().nextDay(-1); }},
		{"tab_player.next_tab", "HotkeyNextTab", [] { Engine::get().nextFile(1); }},
		{"tab_player.prev_tab", "HotkeyPrevTab", [] { Engine::get().nextFile(-1); }},
		{"tab_player.faster", "HotkeyFaster", [] { Engine::get().speedStep(0.05); }},
		{"tab_player.slower", "HotkeySlower", [] { Engine::get().speedStep(-0.05); }},
		{"tab_player.speed_reset", "HotkeySpeedReset", [] { Engine::get().update([](tabobs::Settings &s) { s.speed = 1; }); }},
		{"tab_player.loop", "HotkeyLoop", [] { Engine::get().update([](tabobs::Settings &s) { s.loop = !s.loop; }); }},
		{"tab_player.metronome", "HotkeyMetronome", [] { Engine::get().update([](tabobs::Settings &s) { s.metronome = !s.metronome; }); }},
		{"tab_player.count_in", "HotkeyCountIn", [] { Engine::get().update([](tabobs::Settings &s) { s.countIn = !s.countIn; }); }},
		{"tab_player.notes", "HotkeyNotes", [] { Engine::get().update([](tabobs::Settings &s) { s.showNotes = !s.showNotes; }); }},
	};
	return list;
}

std::string configPath(const char *file)
{
	char *p = obs_module_config_path(file);
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

void hotkeyPressed(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || !data)
		return;
	try {
		static_cast<Hotkey *>(data)->action();
	} catch (...) {
		blog(LOG_WARNING, "[tab-player] hotkey action failed");
	}
}

void registerHotkeys()
{
	obs_data_t *saved = obs_data_create_from_json_file_safe(configPath("hotkeys.json").c_str(), "bak");
	for (auto &h : hotkeys()) {
		h.id = obs_hotkey_register_frontend(h.name, obs_module_text(h.textKey), hotkeyPressed, &h);
		if (saved) {
			obs_data_array_t *arr = obs_data_get_array(saved, h.name);
			obs_hotkey_load(h.id, arr);
			obs_data_array_release(arr);
		}
	}
	obs_data_release(saved);
}

void saveHotkeys()
{
	obs_data_t *d = obs_data_create();
	for (auto &h : hotkeys()) {
		if (h.id == OBS_INVALID_HOTKEY_ID)
			continue;
		obs_data_array_t *arr = obs_hotkey_save(h.id);
		obs_data_set_array(d, h.name, arr);
		obs_data_array_release(arr);
	}
	obs_data_save_json_safe(d, configPath("hotkeys.json").c_str(), "tmp", "bak");
	obs_data_release(d);
}

void unregisterHotkeys()
{
	for (auto &h : hotkeys())
		if (h.id != OBS_INVALID_HOTKEY_ID) {
			obs_hotkey_unregister(h.id);
			h.id = OBS_INVALID_HOTKEY_ID;
		}
}

bool g_shutDown = false;

void shutdownAll()
{
	if (g_shutDown)
		return;
	g_shutDown = true;
	saveHotkeys();
	unregisterHotkeys();
	tabobs::Engine::shutdown();
}

void onFrontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_EXIT)
		shutdownAll(); // stop the audio thread while sources and the UI still exist
	else if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		tabobs::introduceTabPlayerDock();
	else if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED)
		tabobs::Engine::get().update([](tabobs::Settings &) {}); // refresh the dock's set-up hints
}

} // namespace

bool obs_module_load(void)
{
	try {
		tabobs::Engine::get().start(configPath(""));
		tabobs::registerTabSource();
		registerHotkeys();
		tabobs::createTabPlayerDock();
		obs_frontend_add_event_callback(onFrontendEvent, nullptr);
		blog(LOG_INFO, "[tab-player] loaded");
		return true;
	} catch (const std::exception &e) {
		blog(LOG_ERROR, "[tab-player] failed to load: %s", e.what());
	} catch (...) {
		blog(LOG_ERROR, "[tab-player] failed to load");
	}
	return false;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	shutdownAll();
	blog(LOG_INFO, "[tab-player] unloaded");
}
