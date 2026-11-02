#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Smoke test of the plugin inside a real OBS on Linux or macOS (CI runs it on Linux under a
// virtual display). Starts OBS with a fresh configuration: a scene with the Tab Player and its
// metronome, obs-websocket on, the plugin installed in the user's plugins folder. Then it plays a
// demo tab with the plugin's own hotkey and checks that:
//  - the plugin loaded and drew the tab (a screenshot of the source, saved with --shot),
//  - its audio reached the OBS mixer (the Tab Player fader's level meter),
//  - the video thread kept up and the log has no plugin errors.
//
// node tests/obs-smoke.mjs --obs <obs executable> --plugin <obs-tab-player.so or obs-tab-player.plugin>
//      [--file "Demo 06 - Techniques.gp5"] [--seconds 14] [--shot tab.png]
// On Linux it uses its own config folder (XDG_CONFIG_HOME), so your OBS settings are not touched.
// On macOS OBS always uses ~/Library/Application Support/obs-studio: run it on a CI machine only.
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const args = Object.fromEntries(process.argv.slice(2).reduce((a, v, i, all) => (v.startsWith('--') ? [...a, [v.slice(2), all[i + 1]]] : a), []));
const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const SECONDS = Number(args.seconds || 14);
const PORT = 4466;
const mac = process.platform === 'darwin';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
if (!args.obs || !args.plugin) { console.error('usage: node tests/obs-smoke.mjs --obs <obs> --plugin <built plugin>'); process.exit(2); }

// --- a fresh OBS configuration
const env = { ...process.env };
let root;
if (mac) root = path.join(os.homedir(), 'Library', 'Application Support');
else { root = fs.mkdtempSync(path.join(os.tmpdir(), 'obs-smoke-')); env.XDG_CONFIG_HOME = root; }
const cfg = path.join(root, 'obs-studio');
const write = (rel, text) => { const f = path.join(cfg, rel); fs.mkdirSync(path.dirname(f), { recursive: true }); fs.writeFileSync(f, text); };
write('global.ini', '[General]\nEnableAutoUpdates=false\nLastVersion=537001986\nMaxLogs=10\n');
write('user.ini', '[General]\nFirstRun=true\nConfirmOnExit=false\nHotkeyFocusType=NeverDisableHotkeys\n[BasicWindow]\nSysTrayEnabled=false\nWarnBeforeStoppingRecord=false\n');
write('basic/profiles/Test/basic.ini', '[General]\nName=Test\n[Video]\nBaseCX=1280\nBaseCY=720\nOutputCX=1280\nOutputCY=720\nFPSType=0\nFPSCommon=30\n[Audio]\nSampleRate=48000\nChannelSetup=Stereo\n[Output]\nMode=Simple\n');
write('plugin_config/obs-websocket/config.json', JSON.stringify({ alerts_enabled: false, auth_required: false, first_load: false, server_enabled: true, server_password: '', server_port: PORT }));
write('plugin_config/obs-tab-player/settings.json', JSON.stringify({
  library: path.join(REPO, 'docs', 'demo'), fileId: args.file || 'Demo 06 - Techniques.gp5', theme: 'dark', speed: 1, loop: true, countIn: false,
  metronome: true, horizontal: true, colorStrings: true, dockIntroduced: true,
}));
const item = (name, id) => ({ name, id, visible: true, locked: false, pos: { x: 0, y: 0 }, scale: { x: 1, y: 1 }, rot: 0, align: 5, bounds_type: 0, bounds: { x: 0, y: 0 } });
write('basic/scenes/Test.json', JSON.stringify({
  name: 'Test', current_scene: 'Scene', current_program_scene: 'Scene', scene_order: [{ name: 'Scene' }], groups: [], modules: {},
  current_transition: 'Fade', transition_duration: 300, transitions: [], quick_transitions: [],
  sources: [
    { id: 'scene', versioned_id: 'scene', name: 'Scene', enabled: true, flags: 0, mixers: 0, volume: 1, settings: { id_counter: 2, items: [item('Tab Player', 1), item('Tab Player Metronome', 2)] } },
    { id: 'tab_player_source', versioned_id: 'tab_player_source', name: 'Tab Player', enabled: true, flags: 0, mixers: 255, volume: 1, settings: { width: 1920, height: 400 } },
    { id: 'tab_player_metronome', versioned_id: 'tab_player_metronome', name: 'Tab Player Metronome', enabled: true, flags: 0, mixers: 255, volume: 1, settings: {} },
  ],
}));

// --- the plugin, where OBS 30+ looks for a user's plugins
const plugins = path.join(cfg, 'plugins');
fs.rmSync(path.join(plugins, mac ? 'obs-tab-player.plugin' : 'obs-tab-player'), { recursive: true, force: true });
if (mac) {
  fs.cpSync(path.resolve(args.plugin), path.join(plugins, 'obs-tab-player.plugin'), { recursive: true });
} else {
  const dir = path.join(plugins, 'obs-tab-player');
  fs.mkdirSync(path.join(dir, 'bin', '64bit'), { recursive: true });
  fs.copyFileSync(path.resolve(args.plugin), path.join(dir, 'bin', '64bit', 'obs-tab-player.so'));
  fs.cpSync(path.join(REPO, 'plugin', 'data'), path.join(dir, 'data'), { recursive: true });
}

async function connect() {
  for (let i = 0; i < 120; i++) {
    try {
      const ws = new WebSocket(`ws://127.0.0.1:${PORT}`);
      const pending = new Map();
      const meters = {};
      let n = 0;
      await new Promise((ok, fail) => {
        ws.onerror = fail;
        ws.onmessage = (e) => {
          const m = JSON.parse(e.data);
          if (m.op === 0) ws.send(JSON.stringify({ op: 1, d: { rpcVersion: 1, eventSubscriptions: 1 << 16 } })); // InputVolumeMeters
          else if (m.op === 2) ok();
          else if (m.op === 5 && m.d.eventType === 'InputVolumeMeters') {
            for (const inp of m.d.eventData.inputs) meters[inp.inputName] = Math.max(meters[inp.inputName] || 0, ...inp.inputLevelsMul.map((c) => c[1] || 0));
          } else if (m.op === 7) { pending.get(m.d.requestId)?.(m.d); pending.delete(m.d.requestId); }
        };
      });
      return { meters, close: () => ws.close(), call: (requestType, requestData) => new Promise((ok) => { const id = String(++n); pending.set(id, ok); ws.send(JSON.stringify({ op: 6, d: { requestType, requestId: id, requestData } })); }) };
    } catch { await sleep(500); }
  }
  throw new Error('obs-websocket did not come up (did OBS start?)');
}

const result = { ok: false, checks: {} };
const check = (name, ok, detail) => { result.checks[name] = { ok: !!ok, ...(detail === undefined ? {} : { detail }) }; };
console.log(`Starting ${args.obs}`);
const obs = spawn(args.obs, ['--collection', 'Test', '--profile', 'Test', '--disable-updater', '--disable-shutdown-check', '--multi'], { env, stdio: 'ignore' });
try {
  const ws = await connect();
  await sleep(3000);
  const inputs = (await ws.call('GetInputList', {})).responseData?.inputs || [];
  check('sources load', inputs.some((i) => i.inputKind === 'tab_player_source') && inputs.some((i) => i.inputKind === 'tab_player_metronome'), inputs.map((i) => i.inputKind));
  const hotkey = await ws.call('TriggerHotkeyByName', { hotkeyName: 'tab_player.play_pause' });
  check('play hotkey', hotkey.requestStatus?.result, hotkey.requestStatus);
  await sleep(SECONDS * 1000);
  const shot = await ws.call('GetSourceScreenshot', { sourceName: 'Tab Player', imageFormat: 'png' });
  const png = Buffer.from((shot.responseData?.imageData || '').split(',')[1] || '', 'base64');
  if (args.shot && png.length) fs.writeFileSync(args.shot, png);
  // an empty or blank picture compresses to a few KB; a drawn tab is much bigger
  check('tab drawn', png.length > 15000, `${png.length} bytes`);
  check('guitar audio in the mixer', (ws.meters['Tab Player'] || 0) > 0.001, ws.meters['Tab Player']);
  check('metronome in the mixer', (ws.meters['Tab Player Metronome'] || 0) > 0.001, ws.meters['Tab Player Metronome']);
  const stats = (await ws.call('GetStats', {})).responseData || {};
  // CI machines have no GPU (OBS renders in software), so allow a few skipped frames there;
  // tests/obs-native-e2e.mjs checks for none on a real GPU
  const skipped = (stats.renderSkippedFrames || 0) / Math.max(1, stats.renderTotalFrames || 0);
  check('video keeps up', skipped < 0.05 && stats.activeFps > 29, { renderSkippedFrames: stats.renderSkippedFrames, renderTotalFrames: stats.renderTotalFrames, activeFps: stats.activeFps });
  await ws.call('TriggerHotkeyByName', { hotkeyName: 'tab_player.play_pause' });
  ws.close();
} catch (e) {
  check('run', false, String(e.message || e));
} finally {
  obs.kill('SIGTERM');
  await sleep(3000);
  if (obs.exitCode === null) obs.kill('SIGKILL');
}
const logs = path.join(cfg, 'logs');
const log = fs.existsSync(logs) ? fs.readdirSync(logs).sort().map((f) => fs.readFileSync(path.join(logs, f), 'utf8')).join('\n') : '';
const ours = log.split(/\r?\n/).filter((l) => /tab-player/i.test(l));
check('plugin loaded', ours.some((l) => l.includes('[tab-player] loaded')));
check('video drawn while playing', ours.some((l) => l.includes('perf: video')));
check('no plugin errors', !ours.some((l) => /error|failed|cannot/i.test(l)), ours.filter((l) => /error|failed|cannot/i.test(l)));
result.log = ours.slice(0, 20);
result.ok = Object.values(result.checks).every((c) => c.ok);
console.log(JSON.stringify(result, null, 2));
process.exit(result.ok ? 0 : 1);
