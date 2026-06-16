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
| Plugin scaffold | Complete — APVTS params, state save/restore, stereo in/out effect |
| UI | Complete — 420×384 editor: style/density/follow/sound buttons, energy meter, BPM, volume, sample loader |
| Pattern engine | Basic — 3 styles × 3 densities, 16-step bitmask grid, 1 bar |
| Sound sources | **Synth** (built-in procedural voices, default) **or Samples** (Salamander WAV) |
| Host sync | **BPM + ppq** — locks to the DAW bar line when playing (B3) |
| "Adaptive" behaviour | **Follow mode** — density tracks the energy of the effect's input (guide) |
| Tests | JUCE UnitTest suite (DrumPattern, DrumSampler, EnergyAnalyzer, AdaptiveDrummer, DrumSynth) via CTest |
| CI | GitHub Actions — Linux + Windows build, tests, and pluginval |

In short: the plugin lives up to its name and runs as a product. High-priority
bugs are fixed, host timing is ppq-locked, it is audible out of the box (synth)
or with Salamander samples, the adaptive Follow feature is in, and a
regression-test net + Linux/Windows CI + pluginval guard it. Remaining work is
minor polish (A2 caching) and musicality (Phase 4).

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

- **A1 · Med — ✅ FIXED — VST3 sample auto-load path.**
  `prepareToPlay` used to resolve samples relative to `File::currentApplicationFile`
  (the host exe in a DAW), so auto-load only worked for Standalone. Now the last
  successfully-loaded folder is remembered in plugin state and reloaded on restore,
  and `autoLoadSamples()` tries that folder first, then assets next to the plugin
  binary (`currentExecutableFile`), then next to the host/standalone exe.

- **A2 · Low — pattern rebuilt every block.**
  `processBlock` calls `setStyle`/`setDensity` unconditionally
  (`src/PluginProcessor.cpp:85`), and `setStyle` rebuilds the whole pattern via
  `loadStyle`. **Fix:** cache the current style/density and rebuild only on change
  (also needed before adding per-pattern randomisation/fills).

- **A3 · Low — ✅ FIXED — volume zipper noise.**
  Output gain now ramps through a `juce::SmoothedValue` (20 ms) instead of being
  applied raw, so moving the volume no longer zippers.

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
2. ✅ **A1** (VST3 sample path + remembered kit) and **B3** (host ppq lock) fixed;
   **A3** (volume zipper) smoothed via `SmoothedValue`.
3. ✅ Unit tests (`DrumPattern`, `DrumSampler`/B1, `EnergyAnalyzer`,
   `AdaptiveDrummer`/ppq, `DrumSynth`) via CTest, plus **CI on Linux + Windows**
   with **pluginval** validation.
4. ⏳ **D1** (velocity-layers-vs-docs) and **A2** (per-block pattern rebuild) — still open.

### Phase 3 — The "Adaptive" feature ✅ DONE
1. ✅ New `EnergyAnalyzer`, **written fresh** for the plugin's audio-thread
   context (winband used only as a conceptual reference): guide RMS → dB →
   attack/release envelope → 0..1 energy.
2. ✅ The effect's audio input is the guide; it is analysed every block.
3. ✅ Energy → density with a hysteresis band; a **Follow** toggle switches
   between manual and adaptive density, and the UI shows a live energy meter.

### Sound engine — Synth ✅ DONE
A built-in **`DrumSynth`** (procedural kick/snare/hi-hat/crash/ride/tom) is an
alternative to the sampler, chosen via the **Sound** parameter and the default,
so the plugin is audible with no Salamander download. The sampler and synth
share one step-trigger clock (`DrumStepClock.h`) for identical timing.

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

_B1, C1, A1, B3, A3, the **Synth** sound source, a Standalone **Play/Stop**
transport, the test/CI net (Linux + Windows build, tests, pluginval) and Phase 3
(Follow mode) are done. The plugin is an **audio effect / generator** (`Fx|Instrument`),
so it loads in effect hosts including Adobe Audition's Effects Rack; insert it on a
track (the track audio is the Follow guide). Remaining:_

1. **Standalone tempo control** — the BPM display is read-only; add an editable
   tempo for the Standalone (it currently runs at the `bpm` parameter, default 120).
2. **A2** — cache style/density and rebuild the pattern only on change (needed
   before per-pattern randomisation/fills).
3. **D1** — per-step velocity & accents (Phase 4).
4. Phase 4 musicality: fills, humanisation, more styles, MIDI-output mode.
