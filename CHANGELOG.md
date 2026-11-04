# Changelog

Downloads for every version are on the
[Releases page](https://github.com/bask209/obs-tab-player/releases), each with SHA-256 checksums
and a build-provenance attestation.

## 0.6.0 (2026-11-03)

A tidier Tab Player panel.

- **Pages:** the player (title, star, position, Play) stays pinned at the top. Below it are four
  pages: **Tabs** (search, Starred and the list), **Practice** (tempo, Loop, Count-in, Metronome,
  loop bars), **Look** and **Sound**. The list scrolls by itself and never moves the rest of the panel.
- **Reset to defaults** on the Practice, Look and Sound pages puts just that page's settings back.
- **The mouse wheel never changes a setting:** scrolling over a slider, a number or a list of
  choices scrolls the page instead.
- **Starts on your last favourite:** when OBS starts, the panel opens the starred tab you played
  most recently. **★ Starred** is remembered between starts.
- **Removed:** the "Up next" button (the ⏭ button still goes to the next day).

## 0.5.2 (2026-11-03)

- **Tab Player panel fits its dock:** a long tabs-folder path no longer makes the panel wider
  than the dock (it is shortened in the middle; hover it for the full path), and there is no
  sideways scrolling. In a narrow dock the tempo buttons, Loop / Count-in / Metronome and the
  Look checkboxes wrap onto more lines, and the tab list drops "played" and the tempo range
  before it cuts the tab's name.

## 0.5.1 (2026-11-02)

A polish release on top of 0.5.0. The engine and the drawing are unchanged.

- **Stars:** clicking or double-clicking the ☆ in the tab list only stars or unstars the tab.
  It never opens or plays it.
- **Source properties:** the hint now points to the **Docks** menu, where OBS 30 and newer keep
  the Tab Player panel (it said View › Docks).
- **Readme in every download:** links to the new
  [Getting started guide](https://bask209.github.io/obs-tab-player/start.html), and the Windows
  readme says where the six practice tabs are.
- **Website:** a step-by-step Getting started guide with screenshots and fixes for common
  problems, and plain-text summaries for AI assistants (`llms.txt`, `llms-full.txt`).

## 0.5.0 (2026-11-02)

Windows, macOS and Linux.

- **Downloads:** Windows setup.exe or zip; macOS zip (Apple silicon and Intel) with
  Install.command; Linux tar.gz with install.sh, or a .deb. None needs administrator rights. The
  Mac build is new and not yet tried on a real Mac.
- **One renderer everywhere:** the tab is drawn with Qt's painter, which OBS ships on every system,
  instead of Direct2D.
- **Tested inside OBS on Linux** on every build, as on Windows.

## 0.4.0 (2026-11-02)

- **Every Guitar Pro symbol drawn and played:** grace notes, whammy bar, harmonics, trills,
  tremolo picking, strums, tap, slap and pop, accents, staccato, dynamics, fermatas, and the
  segno, coda, fine, D.C., D.S. and To Coda jumps in Guitar Pro's order.
- **Stars and practice history** in the Tab Player panel, per folder.
- **Website:** open your own tabs in the browser, on the plugin's own engine.
- **Removed:** the browser edition (Lua script and browser overlay). One engine, the plugin.

## 0.3.0 (2026-11-02)

First public release: the native OBS plugin for Windows.
