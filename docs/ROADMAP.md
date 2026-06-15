# Adaptive Drummer — Status & Roadmap

_Last updated: 2026-06-15 · applies to v1.0.0_

This document is a situation sketch (an honest assessment of what exists today)
followed by a phased plan for where to take the plugin next. Line references point
at the code as of v1.0.0 and are meant as starting points, not exact anchors.

---

## 1. Situation sketch — what exists today

The repository contains a working JUCE plugin that **builds and runs** (VST3 +
Standalone; verified to compile against JUCE master and render its editor headless).

| Area | State |
|---|---|
| Plugin scaffold | Complete — APVTS params, state save/restore, stereo synth bus |
| UI | Complete — 420×300 editor: style/density buttons, host-synced BPM label, volume knob, sample loader |
| Pattern engine | Basic — 3 styles × 3 densities, 16-step bitmask grid, 1 bar |
| Sample engine | Basic — loads one WAV per voice folder, mixes 6 voices |
| Host sync | Partial — reads host **BPM** only (no ppq; B3 still open) |
| "Adaptive" behaviour | **Follow mode** — density tracks guide-track energy via a sidechain input |
| Tests | JUCE UnitTest suite (DrumPattern, DrumSampler, EnergyAnalyzer), run via CTest |
| CI | GitHub Actions — Linux build + tests |

In short: the plugin now lives up to its name. The two high-priority audio
bugs are fixed, a regression-test net + CI guards the timing maths, and the
**adaptive Follow feature** (Phase 3) is in. Remaining work is correctness
polish (B3 ppq sync, A1 sample path) and musicality (Phase 4).

---

## 2. Critical assessment

Priorities: **High** = audible/crash risk, fix first · **Med** = correctness/UX ·
**Low** = polish.

### Audio correctness

- **B1 · High — ✅ FIXED — gaps on every sustained sample.**
  `DrumSampler::mixVoices` always starts copying at `v.triggerOffset`
  (`src/drummer/DrumSampler.cpp:138`). For a note that started in a *previous*
  block and is still ringing, `triggerOffset` still holds its original in-block
  offset, so each subsequent block re-skips that many samples → a recurring gap on
  any sample longer than one buffer (i.e. essentially all of them). **Fix:** set
  `triggerOffset = 0` once the first block has been consumed.

- **B2 · Med — dropped hits / no tail overlap.**
  Each voice has a single `playPos`/`triggerOffset`
  (`src/drummer/DrumSampler.cpp:126`). If a voice is triggered twice within one
  process block (large buffers or high BPM where `stepLen < blockSize`), the second
  trigger overwrites the first and the first hit is lost. A voice also cannot
  overlap its own decay. **Fix:** a small per-voice voice-pool (e.g. 4 voices) or
  at minimum collect all triggers for the block and render each.

- **B3 · Med — no host position sync (drift).**
  `processBlock` reads host **BPM** but not **ppqPosition**
  (`src/PluginProcessor.cpp:76`); the drummer free-runs from its own
  `playheadSample` counter (`src/drummer/AdaptiveDrummer.cpp:47`). It therefore
  does not line up to the DAW's bar 1 and drifts over a session. **Fix:** when the
  host is playing, derive the step index from `ppqPosition` instead of an internal
  counter.

- **B4 · Low — integer-truncation tempo drift.**
  `getLengthInSamples` and `stepLen = patternLen / kSteps` both truncate
  (`src/drummer/DrumPattern.cpp:16`), dropping up to `kSteps` samples per bar.
  Subsumed by fixing B3 (sample-accurate positioning from ppq).

### Concurrency

- **C1 · High — ✅ FIXED — data race on sample load.**
  `DrumSampler::loadSamples` runs on the message thread (editor "Load samples…"
  button) and resizes/overwrites `voices[].sample` while `processBlock` reads those
  buffers on the audio thread. No lock or atomic swap → race and possible crash.
  **Fix:** load into a temporary, then swap under a `SpinLock`/`CriticalSection`
  the audio thread try-locks, or hand the loaded set to the audio thread via a
  lock-free FIFO.

### Correctness / API

- **A1 · Med — VST3 sample auto-load path is wrong.**
  `prepareToPlay` resolves samples relative to `File::currentApplicationFile`
  (`src/PluginProcessor.cpp:56`). In a DAW that is the **host** executable, not the
  plugin bundle, so auto-load only works for Standalone. **Fix:** resolve relative
  to the plugin module, or use a user/app-data location, or remember the
  last-used folder in plugin state.

- **A2 · Low — pattern rebuilt every block.**
  `processBlock` calls `setStyle`/`setDensity` unconditionally
  (`src/PluginProcessor.cpp:85`), and `setStyle` rebuilds the whole pattern via
  `loadStyle`. **Fix:** cache the current style/density and rebuild only on change
  (also needed before adding per-pattern randomisation/fills).

- **A3 · Low — volume zipper noise.**
  Gain is applied raw (`src/PluginProcessor.cpp:94`). **Fix:** `SmoothedValue`.

### Docs / feature mismatch

- **D1 — velocity layers are claimed but absent.**
  The README describes velocity-layer selection (PP/MP/MF/F/FF), but
  `DrumSampler::loadFirstWavInDir` (`src/drummer/DrumSampler.cpp:88`) loads only the
  first WAV; patterns carry no velocity/accent data — every hit plays at full
  level. **Fix:** implement velocity (see Phase 4) or correct the README.

- **D2 — "Adaptive" is aspirational.** Nothing adapts yet; the name promises the
  Phase 3 feature.

### Process

- **P1 — ✅ DONE — tests + CI.** A JUCE UnitTest suite covers the pattern/timing
  maths (`DrumPatternTest`), the sampler incl. a B1 regression (`DrumSamplerTest`),
  and the adaptive mapping (`EnergyAnalyzerTest`); GitHub Actions builds + runs them
  on Linux.
- **P2 — deprecation warnings:** `juce::Font(float)` constructors are deprecated in
  JUCE 8; the editor uses them throughout.

---

## 3. Roadmap

### Phase 2 — Correctness & robustness
1. ✅ Fixed the two **High** items: **B1** (sample gaps) and **C1** (load race).
2. ⏳ **B3** (host ppq sync) and **A1** (VST3 sample path) — still open.
3. ✅ Added `DrumPatternTest`, `DrumSamplerTest` (with a B1 regression) and
   `EnergyAnalyzerTest`, plus **GitHub Actions CI** (Linux build + tests).
4. ⏳ **D1**: velocity-layers-vs-docs — still open.

### Phase 3 — The "Adaptive" feature ✅ DONE
1. ✅ New `EnergyAnalyzer`, **written fresh** for the plugin's audio-thread
   context (winband used only as a conceptual reference): guide RMS → dB →
   attack/release envelope → 0..1 energy.
2. ✅ Added a **Sidechain** input bus; the guide is analysed every block.
3. ✅ Energy → density with a hysteresis band; a **Follow** toggle switches
   between manual and adaptive density, and the UI shows a live energy meter.

### Phase 4 — Musicality
1. Per-step **velocity & accents**; humanisation (small timing/velocity jitter).
2. **Fills** every N bars; intro/ending variations.
3. More styles (Funk, Brushes already exist in winband); multiple variations per
   style.
4. **MIDI-output mode** — emit notes so users can drive their own drum sampler.

### Phase 5 — UX & distribution
1. Custom `LookAndFeel`; a visual step-sequencer / pattern display.
2. Preset system.
3. Move hard-coded colours into a shared theme (mirror winband
   `assets/theme/dark.json`).
4. AU/AAX formats; macOS notarisation; installers.

---

## 4. Suggested next steps

_B1, C1, the test/CI net and Phase 3 (Follow mode) are done. Remaining:_

1. **B3** — derive the step index from host `ppqPosition` so the drummer locks to
   the DAW timeline (also subsumes B4's truncation drift).
2. **A1** — resolve the auto-load sample path relative to the plugin module / a
   user location, and remember the last-used folder in plugin state.
3. **D1** — implement per-step velocity/accents (Phase 4) or correct the README.
4. **Windows CI** job to mirror the Linux one (the product's primary platform).
5. Phase 4 musicality: fills, humanisation, more styles, MIDI-output mode.
