# Fable 5 handoff — context pack

> **Purpose.** Everything Claude Fable 5 needs to (a) do a deep, real-time-safety-
> aware review of this codebase and (b) design an architecture + phased roadmap to
> evolve it toward the product goal below. This pack was assembled by Opus 4.8 so
> Fable does not have to spend its (expensive) budget rediscovering context.
> Companion file: [`FABLE_PROMPT.md`](FABLE_PROMPT.md) — the actual assignment.

---

## 1. Product goal

Build a VST3 (and Standalone) plugin that does, in a DAW, what **GarageBand's
Drummer** does. Three features define "done":

1. **Style choice** — pick between distinct drumming styles.
2. **A 2-D grid position** — a puck on a pad whose axes are **intensity**
   (soft ↔ loud/busy) and **complexity** (simple ↔ complex). Moving the puck
   continuously reshapes the groove.
3. **Follow another track** — adapt the groove to a *user-chosen different track*
   (a guide), not just react in the abstract.

The current build already has (1) in basic form and a **1-D** stand-in for (2).
Feature (3) exists but follows the *plugin's own input*, not a separate track.
The whole point of the Fable pass is to design the clean jump from where we are to
this goal without a wholesale rewrite.

---

## 2. Where the code is today (accurate as of this handoff)

Working JUCE plugin, **builds and runs** on Linux + Windows CI, VST3 + Standalone,
audible out of the box. It is packaged as an **audio effect / generator**
(`Fx|Instrument`), stereo in/out, in-place.

### 2.1 Signal flow (per `processBlock`)

```
host/track audio ──▶ main input ──▶ EnergyAnalyzer (guide, if Follow on)
                                         │  RMS → dB → attack/release env → energy 0..1
                                         ▼
                             energy → Density (hysteresis band)
buffer.clear()  ◀── output is the generated drums (input is consumed as guide)
                                         │
   DrumStepClock (forEachStepTrigger) ── walks samples, fires voice hits per 16th step
                                         │
              DrumSampler  OR  DrumSynth ── add drum audio into the output buffer
                                         │
                       SmoothedValue gain (volume) ── applyGain
```

### 2.2 Key files

| File | Responsibility |
|---|---|
| `src/PluginProcessor.{h,cpp}` | APVTS params, bus layout (stereo in/out), host BPM+ppq sync, Follow gating, transport (`play` + host), state I/O |
| `src/PluginEditor.{h,cpp}` | 420×384 UI: style/density/follow/sound buttons, energy meter, editable BPM, volume, sample loader |
| `src/drummer/DrumPattern.{h,cpp}` | The pattern model — **see §3, this is the crux** |
| `src/drummer/DrumStepClock.h` | `forEachStepTrigger(...)` — shared sample-accurate 16th-note clock for both engines |
| `src/drummer/AdaptiveDrummer.{h,cpp}` | Orchestrator: holds pattern + both engines, ppq→playhead, `processBlock` adds audio |
| `src/drummer/DrumSampler.{h,cpp}` | Salamander WAV loader + playback |
| `src/drummer/DrumSynth.{h,cpp}` | Procedural kick/snare/hihat/crash/ride/tom (default; no samples needed) |
| `src/drummer/EnergyAnalyzer.{h,cpp}` | Guide energy → adaptive density (envelope + hysteresis) |
| `tests/` | JUCE UnitTest suite (DrumPattern, DrumSampler incl. a B1 regression, EnergyAnalyzer, AdaptiveDrummer/ppq, DrumSynth), run via CTest |

### 2.3 Parameters (APVTS, `createParameterLayout` in `PluginProcessor.cpp`)

`style` (Rock/Jazz/Electronic) · `density` (Sparse/Medium/Full) · `bpm` (40–240,
editable in UI) · `volume` (0–1) · `follow` (bool) · `source` (Synth/Samples) ·
`play` (bool transport). All automatable, saved to session state.

### 2.4 Real-time model (respect this in every proposal)

- `processBlock` runs on the **audio thread**: no allocation, no locks, no file I/O.
- Pattern data is read on the audio thread; anything that *builds* patterns or loads
  samples must happen off-thread and be handed over safely (the sample-load race
  **C1** was fixed with an atomic/locked swap — reuse that pattern).
- Both engines share one clock (`DrumStepClock.h`) so timing stays identical.

---

## 3. The crux: the pattern model and the 2-D gap

`DrumPattern` (`src/drummer/DrumPattern.h:49`) stores:

```cpp
uint8_t layers[3][kSteps];   // [density][step], kSteps = 16, one bar
```

Each step is a **voice bitmask** (`Kick=0x01 … Tom=0x20`). A style is three
hand-authored 16-step layers (`buildRock/buildJazz/buildElectronic`), and
`setDensity` picks **one** of the three layers. The header comment
(`DrumPattern.h:8`) is explicit:

> *"three density layers (Sparse / Medium / Full) that map to the GarageBand
> Drummer concept of soft/simple → loud/complex."*

**That is the gap in one sentence:** the product needs two independent axes
(intensity **and** complexity), but the code collapses them into a single 3-step
`density`. There is also **no velocity/accent data** — every hit plays at full
level (issue **D1**), so the "intensity" axis currently has nothing to modulate
besides which hits exist.

---

## 4. Known issues to verify and extend (from `ROADMAP.md`)

Fable should **independently verify** these and find more (especially RT-safety):

| ID | Sev | Status | One-liner |
|---|---|---|---|
| B1 | High | ✅ fixed | Sustained samples re-skipped `triggerOffset` each block → gaps |
| B2 | Med | **open** | One `playPos` per voice → double-trigger within a block drops a hit; no decay overlap (no polyphony) |
| B3 | Med | ✅ fixed | ppq lock to DAW bar line |
| B4 | Low | **open** | Integer truncation in `getLengthInSamples`/`stepLen` → tempo drift (`DrumPattern.cpp`, `DrumStepClock.h:28`) |
| C1 | High | ✅ fixed | Sample-load data race — fixed via safe swap (reuse this pattern for dynamic pattern regen) |
| A2 | Low | **open** | `processBlock` calls `setStyle`/`setDensity` every block → full pattern rebuild every block (`PluginProcessor.cpp`). Must fix before dynamic/generative patterns |
| D1 | — | **open** | Velocity/accents claimed but absent — patterns are pure on/off masks |
| P2 | — | open | `juce::Font(float)` deprecations in the editor |

`B2`, `A2`, and `D1` are directly on the path to the 2-D groove engine, so they
should be folded into the design, not treated as separate chores.

---

## 5. Strawman designs (starting points for Fable to critique/replace)

These are **deliberately rough** — Opus authored them to focus Fable, not to
constrain it. Fable should score them, improve, or discard.

### 5.1 The 2-D intensity × complexity engine

- **Option A — discrete cell table.** An N×M grid of hand-authored 16-step patterns
  per style; puck selects nearest cell (or blends neighbours). Authorable, musically
  safe, but non-continuous and heavy to author.
- **Option B — parametric/probabilistic generator.** Per-voice step probabilities as
  functions of `(complexity, intensity, style)`: kick density, snare backbeat + ghost
  notes, hi-hat subdivision (8th→16th), accents, fills. Continuous and compact, needs
  careful musical tuning and a **seedable RNG** so output is deterministic and
  reproducible.
- **Option C — layered embellishment.** A base groove plus embellishment layers
  progressively enabled by `complexity`; `intensity` drives velocity/dynamics and hit
  probability. Between A and B.

Cross-cutting requirements the design must satisfy:
- Extend the pattern model from `uint8_t` masks to **per-step, per-voice velocity**
  (solves D1; gives the intensity axis something to move).
- Regenerate patterns **only on parameter change**, off the audio thread, and swap in
  RT-safely (solves A2; reuse the C1 swap pattern).
- Decide the fate of the `density` parameter: replace with `intensity` + `complexity`,
  or keep `density` and add `complexity`? Include **state-migration** for old sessions.

### 5.2 Follow *another* track

- Today: `EnergyAnalyzer` reads the plugin's **own** main input.
- Target: an **optional sidechain input bus** the user routes a chosen track into; the
  analyzer reads the sidechain instead. (Note: an earlier version *had* a sidechain
  bus; it was removed during the effect conversion — re-introducing it as the guide is
  the natural path.)
- Map analysis → axes: e.g. RMS/energy → **intensity**; onset-rate / spectral-flux →
  **complexity**. Reuse the existing envelope + hysteresis smoothing.
- **DAW reality to design around:** VST3 sidechain routing differs per host (Live,
  Reaper, Cubase…), some hosts/Adobe Audition support it poorly or not at all, and
  Standalone has no "other track." The design needs a graceful fallback (follow own
  input, or manual) when no sidechain is connected.

---

## 6. Open questions for Fable to resolve

1. Which 2-D model (A/B/C or a hybrid) best preserves authored musical quality while
   being continuous — and how is it tuned/authored?
2. Is Phase 4 **audio-first or MIDI-out-first**? (MIDI-out changes the engine's
   output contract and may be easier to make musical.)
3. Sidechain design: bus topology, per-DAW routing notes, which features drive which
   axis, and the no-sidechain fallback.
4. Where does dynamic pattern generation run, and how is the swap made RT-safe?
5. Parameter/state migration: `density` → `intensity` + `complexity` without breaking
   saved sessions/automation.
6. Velocity/accent data model and how both engines (sampler + synth) consume it.

---

## 7. Constraints & acceptance criteria for the deliverable

- **Evolve, don't rewrite.** Reuse the existing structure; keep it building on the
  current CI at every step.
- **RT-safety is non-negotiable.** Every proposed change names its audio-thread
  implications.
- **Keep CI green:** Linux + Windows build, JUCE UnitTests via CTest, **pluginval
  --strictness-level 10**.
- **Implementable by a cheaper model.** The roadmap must be dependency-ordered and
  broken into steps small enough that Opus 4.8 can execute the mechanical parts, each
  with: files touched · RT-safety note · test to add · risk.

---

## 8. Build & test (reference)

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DADAPTIVE_DRUMMER_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
# pluginval (CI uses strictness 10):
pluginval --strictness-level 10 --validate "build/AdaptiveDrummer_artefacts/Release/VST3/Adaptive Drummer.vst3"
```

Repo: JUCE is a submodule under `third_party/JUCE`. CI: `.github/workflows/ci.yml`
(push to `main` + PRs). Full status/roadmap: [`ROADMAP.md`](ROADMAP.md). Next-phase
checklist: [`TODO.md`](TODO.md).
</content>
</invoke>
