# OBS Tab Player

**Scrolling guitar tabs with synced sound, right inside OBS Studio.**

Put a Guitar Pro tab next to your camera, press play, and practise, teach or perform. The cursor
follows the music, the audio goes into your recordings and streams through OBS's own mixer, and you
pick tabs, tempo, loops and the metronome from an OBS dock or global hotkeys.

[**Website and live demo**](https://bask209.github.io/obs-tab-player/) ·
[**Download**](https://github.com/bask209/obs-tab-player/releases/latest) ·
[Report a problem](https://github.com/bask209/obs-tab-player/issues)

![OBS Tab Player](docs/og-image.png)

- **Formats:** Guitar Pro 7/8 (`.gp`), Guitar Pro 6 (`.gpx`), Guitar Pro 3–5 (`.gp3`, `.gp4`, `.gp5`).
  Any string count, so 6/7-string guitar and bass work.
- **Playback:** built-in string synthesizer, so no soundfont download. Bends, slides, hammer-ons, palm
  mutes, let-ring, vibrato, harmonics, dead notes, ties, tuplets, repeats and alternate endings.
- **Practice:** tempo as a speed multiplier or exact BPM, tempo-range presets read from the file's
  notes, bar loops, count-in and metronome.
- **Library:** search hundreds of tabs by number (`4`), range (`10-20`) or topic (`sweep`). Next and
  previous by day or file.
- **Overlay:** six themes (light, dark, glass, transparent, neon, paper), coloured strings, bold
  mode, four fonts, a page or single-line layout, and the file's notes (description, tip, tempo range)
  above the tab.
- **Native to OBS:** one Lua script runs everything: a tiny web server, the library scanner and
  14 hotkeys. There are no other programs, runtimes or accounts.
- **Light:** about 180 KB in total with zero dependencies. See [Performance](#performance).
- **License:** Apache-2.0. All code is original, with no third-party runtime components.

## Install (Windows, about one minute)

1. Download `obs-tab-player-<version>.zip` from
   [Releases](https://github.com/bask209/obs-tab-player/releases/latest) and extract it somewhere
   permanent, for example `Documents\OBS Tab Player`.
2. In OBS: **Tools › Scripts › +** and choose `tab-player.lua` from that folder.
3. In the script's settings: pick your **Tab folder**, then click **Add overlay to current scene**.
   This adds a Browser Source called *Tab Player* (960×540, *Control audio via OBS* on).
4. **Docks › Custom Browser Docks…** and add the dock:
   Dock Name `Tab Player`, URL `http://127.0.0.1:8737/dock.html`. Then click Apply.

Pick a tab in the dock and press ▶. Hotkeys are under **Settings › Hotkeys** (search "Tab Player").
The `demo-tabs` folder in the zip has five original exercises you can try straight away.

Requirements: OBS Studio 30 or newer (tested on 32.2, Windows 11). macOS and Linux: the overlay and
dock are platform independent, and the script includes a POSIX socket backend, but that backend is
**not yet tested**. Reports are welcome.

### Sizing it next to your camera

The overlay fills whatever size you give the Browser Source. Good starting points are 960×540 beside a
camera, or 1280×360 as a strip along the bottom. Use **Look › Size** in the dock to scale the
notation, **Layout › One line** for a ticker-style strip, and **Theme › Transparent** or **Glass** to
lay it over video. Keep the source at 30 FPS (the default).

### Audio

The overlay plays through its own Browser Source, so it appears in the **Audio Mixer** as *Tab Player*
and is recorded and streamed like any other source. If the cursor feels early or late against the
sound, use **Sound › Cursor offset** in the dock. It moves only the cursor, never the audio.

### Hotkeys and remote control

Play/Pause · Stop · Restart · Next/Previous day · Next/Previous tab · Faster/Slower (5%) · Speed 1× ·
Loop · Metronome · Count-in · Exercise text.

The same commands are available over HTTP for Stream Deck, Touch Portal or scripts:
`http://127.0.0.1:8737/api/command/<name>`, for example `.../api/command/playPause` or
`.../api/command/speedUp?step=0.1`.

Dock keyboard shortcuts (when the dock has focus): `Space` play/pause, `/` search, `↑ ↓ Enter`
pick, `Ctrl+Enter` pick and play, `[ ]` previous/next day, `+ −` tempo, `L` loop, `M` metronome,
`C` count-in.

## How it works

```
OBS Studio
├── tab-player.lua  (Tools › Scripts)
│     ├─ tiny HTTP server on 127.0.0.1:8737 (LuaJIT FFI sockets, polled on OBS's tick, never blocks)
│     ├─ scans the tab folder, remembers the player state, registers hotkeys
│     └─ relays commands and status between pages (Server-Sent Events)
├── Browser Source  → /overlay.html   renders the tab, plays audio, follows the state
└── Custom Dock     → /dock.html      search, transport, tempo, loop, look and sound settings
```

- `player/www/js/core/`: the engine, plain ES modules with no dependencies:
  `gp3to5.js` (binary GP3–5), `gpx.js` (GP6 container), `zip.js` (GP7/8 container), `gpif.js`
  (GP6–8 XML), `xml.js`, `timeline.js` (repeats, tempo map, ties, bends), `render.js` (SVG tab),
  `synth.js` (Karplus-Strong strings via Web Audio), `player.js` (look-ahead scheduler).
- `player/www/js/app/`: the tab view (cursor and scrolling) and shared helpers.
- `player/tab-player.lua`: the OBS side.
- `tools/dev-server.mjs`: the same HTTP API in Node, for development or use outside OBS.

### Scaling up for streaming

Because everything talks over a small local HTTP API, adding these later needs no changes to the
overlay:

- **Chat commands** (`!day 42`, `!tempo 90`): a bot calls `/api/command`. The `set` command accepts any
  state field, for example `{"type":"set","values":{"fileId":"Day 042 - Legato.gp5"}}`.
- **Stream Deck or other controllers:** plain GET URLs (see above).
- **Scene automation:** obs-websocket can switch scenes when playback starts, and the overlay reports
  its status on `/events`.
- **Several layouts:** add more Browser Sources pointing at `/overlay.html`. Add `?muted=1` to extra
  copies so only one of them makes sound.
- **Remote/LAN control:** the server deliberately listens on 127.0.0.1 only. Exposing it would need
  authentication first.

## Performance

Measured in OBS 32.2.2 on Windows 11 (`tests/obs-e2e.mjs`), playing a lesson in a 1280×720 Browser
Source at 30 FPS while recording:

| | CPU (one core) | Memory |
|---|---|---|
| Browser processes, idle | ~1% | ~240 MB (OBS's browser engine, mostly fixed cost) |
| Browser processes, playing | ~4–6% | +7 MB |
| OBS render time per frame | 0.22 ms | |
| Dropped/skipped frames | 0 | |

Engine timings (`npm test`, Node 24):

- A typical exercise renders in about 0.25 ms. Parsing takes under 0.1 ms.
- A deliberately huge 400-bar file (25,600 notes) takes 7 ms to parse, 10 ms to build the timeline
  and 32 ms to render.
- All 688 files of a real 365-day exercise library parse and render in well under 1 ms each on average.

What keeps it light:
- The tab is drawn once as static SVG. While playing, only two small layers move, and only by
  transforms.
- The frame loop runs only while playing.
- The synth renders each needed pitch once into a small 32 kHz buffer. After that, playback is native
  `AudioBufferSourceNode`s.
- The AudioContext is pinned to 48 kHz with "playback" latency.
- The Lua server does a few non-blocking socket calls per OBS tick.

## Development

```bash
npm test
```

```bash
node tools/dev-server.mjs "C:\path\to\tabs"
```

```bash
npm run demo
```

```bash
npm run package
```

```bash
npm run site
```

- `npm test` runs the format round trips, engine, server and performance tests. Set `TAB_LIBRARY` to
  also parse a whole folder of your own tabs.
- `tools/dev-server.mjs` serves the overlay and dock at http://127.0.0.1:8737 in any browser.
- `npm run demo` regenerates `docs/demo/*.gp5`.
- `npm run package` builds `dist/obs-tab-player-<version>.zip`.
- `npm run site` builds the website into `_site/`.

`tests/obs-e2e.mjs` drives a real **portable** copy of OBS: it loads the script, plays a tab while
recording, and reports CPU, memory, frame stats and log errors. See the comments at the top of the
file.

The test suite checks:
- Every demo song written as GP3, GP4, GP5.00, GP5.10, GP6 (`.gpx`, plain and compressed) and GP7
  (`.gp`, stored and deflated) is read back exactly.
- Truncated and invalid files fail cleanly.
- Repeats, alternate endings, triplets, ties, bends and loop ranges produce the right timing.
- The renderer outputs no NaN at any size and gives cursor geometry for every beat, and text from
  files is escaped.
- The synth is in tune to within 5 cents across the neck.
- The HTTP API works, and the path-traversal and DNS-rebinding guards hold.

## License

Apache License 2.0, see [LICENSE](LICENSE). The demo exercises in `docs/demo` are original and covered
by the same license. Guitar Pro is a trademark of its owner. This project is not affiliated with Arobas
Music or the OBS Project.
