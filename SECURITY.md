# Security

## Reporting

Please report security problems privately through GitHub:
**Security › Report a vulnerability** on https://github.com/bask209/obs-tab-player.

## Threat model

The plugin runs inside OBS with the user's rights. The input it does not control is **tab files**:
a file from the internet can be crafted to attack a parser. Everything read from a file is treated
as hostile. The plugin opens no network ports and loads no code or plugins of its own.

## How the code defends itself

- **Bounds-checked parsing.** Every byte read goes through a reader that checks remaining length
  first. Malformed input raises an exception that is caught before it reaches OBS.
- **Hard limits** (`plugin/src/core/limits.hpp`). They cover file size (64 MB), decompressed size
  (128 MB, which stops zip bombs), bars, tracks, strings, beats per bar, bars after repeat
  expansion, playback events, tempo range, bend range and XML nesting depth.
- **Values from files are clamped:** tempo, frets, bends, whammy, tuplets, meters, tuning, harmonic
  positions and fermata lengths. Numbers written as "nan" or "inf" in Guitar Pro 6-8 files are
  ignored. The synth also guards against NaN and infinite playback rates and soft-limits its
  output.
- **Loops always end.** Repeats and D.C./D.S. jumps are taken a bounded number of times, and the
  GPX container reader only moves forward through the file.
- **No regular expressions.** MSVC's `std::regex` is recursive and can overflow the stack on long
  input, so all text scanning is hand-written and linear.
- **No manual memory management.** RAII containers, `std::unique_ptr` / `std::shared_ptr`,
  `ComPtr` for COM, and owning wrappers around OBS objects.
- **Threads.** The audio and render worker threads are always joined on shutdown. Shared state is
  protected by mutexes, and the video thread never waits on long work.
- **No exceptions cross into OBS.** Every OBS callback is wrapped.
- **GPU memory is bounded.** The tab is rendered in 1024 px tiles only where visible, with a cap on
  cached tiles, so huge tabs never request huge textures.
- **Hardened build.** `/W4 /WX /sdl /guard:cf`, linked with `/CETCOMPAT /DYNAMICBASE /NXCOMPAT
  /HIGHENTROPYVA`. The installer needs no administrator rights: the plugin folder then belongs to the user who
  installed it, like any per-user app. Installing with `/ALLUSERS` (as administrator) also
  restricts write access to the plugin folder to administrators.

## Verification

- **Fuzzing:** `tests/fuzz_load.cpp` is a libFuzzer target over every reader, the timeline, the
  layout and the synth, built with AddressSanitizer (`-DTAB_FUZZ=ON`). Runs so far: 2,501,783 inputs
  (10 + 3 + 4 minutes), with no crash, memory error, timeout or out-of-memory.
- **AddressSanitizer:** the full pipeline over 688 real files (parse, timeline, layout, Direct2D
  render, synth) under ASan is clean.
- **Leaks:** `tabtool soak` repeats that pipeline 13,760 times. Private memory stays flat (no growth
  after warm-up).
- **Cross-checked:** the C++ readers produce identical results to the independent JS readers for
  every format and all 688 library files (`tests/native-parity.test.mjs`).
