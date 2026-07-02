# Architecture — 2-D Groove Engine + Follow-Another-Track

> **Status.** Chosen architecture, resolved from the candidate designs solicited in
> [`FABLE_PROMPT.md`](FABLE_PROMPT.md) against the context in
> [`FABLE_HANDOFF.md`](FABLE_HANDOFF.md). This document is the implementation
> contract: it defines the 2-D intensity × complexity engine, the sidechain-based
> Follow feature, the parameter/state migration, and the audio-vs-MIDI decision,
> and it explicitly resolves the six open questions from `FABLE_HANDOFF.md` §6
> (see [§9](#9-resolutions-to-the-six-open-questions)).
>
> **Shape of the design.** Base is the layered/interpolated model (strawman
> Option C evolved into a threshold-gated table), grafted with a seeded-hash
> probability layer, a user-visible groove seed, and a lock-free table mailbox
> for future dynamic tables. Everything is an evolution of the existing files —
> no rewrite.

---

## 1. Overview

```
                      ┌──────────────────────────── message thread ───────────────┐
                      │ APVTS params ──edge-detect──▶ atomics (style idx, seed)    │
                      │ density (deprecated macro) ──▶ intensity+complexity params │
                      └────────────────────────────────────────────────────────────┘
 sidechain bus ──▶ EnergyAnalyzer ──▶ energy → intensity      (atomics, audio thread)
 (or main in,                    └──▶ onset-rate → complexity (dead-band + bar latch)
  or silence)
                      ┌──────────────────────────── audio thread ─────────────────┐
                      │ DrumStepClock (exact-double boundaries, swing)             │
                      │   └─ per step: fire? = threshold gate + seeded-hash prob   │
                      │        vel01  = lerp(velLow, velHigh, intensity^0.7) ± rnd │
                      │   └─ onTrigger(voice, offsetInBlock, velocity01)           │
                      │ DrumSynth / DrumSampler (4-slot polyphonic pools)          │
                      └────────────────────────────────────────────────────────────┘
```

Two continuous axes replace the 3-step `density`:

- **intensity** (0..1) — *how hard*: per-hit velocity re-voicing. Non-structural,
  applied immediately (smoothed ~50 ms).
- **complexity** (0..1) — *how busy*: which cells of the groove table are active.
  Structural per hit, sampled at each step boundary (bar-latched only on the
  Follow path).

Style stays a discrete choice; each style is one immutable `constexpr` table.

---

## 2. The 2-D engine

### 2.1 Data model — `src/drummer/DrumPattern.h`

The `uint8_t layers[3][kSteps]` bitmask array (`DrumPattern.h:49`) is replaced by
a per-voice, per-step cell table. This is the D1 fix (velocity/accents) and the
2-D gap fix in one structure:

```cpp
struct StepCell {                    // 6 bytes, POD
    uint8_t velLow, velHigh;         // velocity at intensity 0 / 1 (0..127; velHigh==0 => never plays)
    uint8_t complexityThreshold;     // fires iff complexity255 >= this
    uint8_t probability;             // 0..255; 255 = deterministic backbone
    int8_t  velRandRange;            // ± humanization via seeded hash
    uint8_t flags;                   // bit0 accent, bit1 ghost, bit2 fill-only (reserved)
};

struct GrooveTable {                 // 6 voices * 16 steps * 6 B = 576 B + tail, per style
    StepCell cells[6][DrumPattern::kSteps];
    uint8_t  swing;                  // 0..255 → 0..~66% delay of odd 16ths (consumed by the clock)
    uint32_t styleSalt;              // mixed into the trigger hash so styles decorrelate
};
```

Design intent of the two velocity columns: **intensity re-voices, it does not
just scale.** A ghost note authored `velLow=18, velHigh=34` stays a ghost at
full intensity; a hi-hat authored `velLow=25, velHigh=110` goes tick → bark.
Accent compression falls out of authoring (flatter `velLow..velHigh` spans),
not a runtime formula.

The three styles are `const` file-scope tables in `DrumPattern.cpp`, authored by
evolving `buildRock/buildJazz/buildElectronic` into table authors. Class name,
`Voice` enum, and `kSteps` are kept; `getLengthInSamples` returns `double`
(B4 fix).

**Mask-equivalent seeding** (the migration invariant): every hit in today's
Sparse layer gets `complexityThreshold = 0`; hits added by Medium get ~100;
hits added by Full get ~190 — all with `probability = 255`. Therefore legacy
`density ∈ {Sparse, Medium, Full}` maps exactly onto
`complexity ∈ {0.2, 0.55, 1.0}` and old sessions sound identical. New musical
territory (ghosts, ornaments) is authored above the legacy range: thresholds
200–240, `probability < 255`, `velRandRange` ±8–15.

`DrumPatternTest`'s raw-mask assertions are restructured **in the same commit**
into structural invariants: kick on steps 0/8 and snare on 4/12 at Rock /
complexity 0.55; hit-count monotonically non-decreasing in complexity;
the active set at Full-point is a superset of the Sparse-point set; crash gated
to step 0.

### 2.2 Firing rule (audio thread, per step boundary)

Integer/float ops only — no allocation, no locks, no RNG state:

```cpp
// complexity255 = uint8 sampled from the complexity atomic at THIS step boundary
fires = cell.velHigh > 0
     && complexity255 >= cell.complexityThreshold
     && (cell.probability == 255
         || hash01(seed ^ table.styleSalt, barIdx, voice, step) * 255.0f
              < cell.probability * probScale(complexity));

vel01 = lerp(velLow, velHigh, powf(intensity, 0.7f)) / 127.0f
      + hashBipolar(seed ^ table.styleSalt, barIdx, voice, step, /*stream=*/1)
          * cell.velRandRange / 127.0f;            // clamped to 0..1
```

- `hash01` is a stateless **SplitMix64**-style avalanche of its inputs mapped to
  [0,1). Zero RNG objects, zero state, bit-exact reproducibility: realtime
  playback == offline bounce == session reload for the same `grooveSeed`. This
  also retires the unseeded `juce::Random` in `DrumSynth`.
- `probScale(complexity)` gently raises ornament probability with complexity so
  the probabilistic layer thickens along the same axis as the threshold layer.
- **complexity is sampled once per step boundary** into a local. Each hit's gate
  decision is made at its own trigger instant, so puck movement is artifact-free
  by construction and maximally responsive. (Bar-latching is reserved for style
  changes and for *followed* complexity — see §3.4/§4.)
- **intensity** is read per block from a relaxed atomic and smoothed via
  `juce::SmoothedValue` (~50 ms, the existing idiom at `PluginProcessor.cpp:75`).
  Non-structural, so it applies immediately.

### 2.3 Clock — `src/drummer/DrumStepClock.h` (prerequisite)

Order is non-negotiable: **`tests/DrumStepClockTest.cpp` lands first** (block
sizes {1, 64, 480, 4096} at 130 BPM / 44.1 kHz; assert exactly 16 firings per
bar and exactly-once across block edges). Then the rewrite:

- `patternLenExact` as `double`; step boundaries are
  `llround(k * patternLenExact / 16)` walked via a `nextStepSample` cursor.
  This kills the phantom 17th step and both per-sample modulos
  (`DrumStepClock.h:33-35`) — the B4 fix.
- **Swing hook**: odd-index boundaries get `+ swing * stepLenExact` before
  rounding (fed from `GrooveTable::swing`).
- Callback signature becomes `onTrigger(int voice, int offsetInBlock,
  float velocity01)`.
- In `AdaptiveDrummer`: free-running `playheadSample` as `double`; re-derive
  from host ppq only when `|ppqDerived − freeRun| > ~2 ms`; split `hasPpq` from
  `hostPlaying` (fixes the playing-without-ppq silence at
  `PluginProcessor.cpp:123-127`); read the host time signature for
  `beatsPerBar`.

### 2.4 Engines consume velocity — `DrumSynth.cpp`, `DrumSampler.cpp`

B2 fix and velocity plumbing in one pass, in both engines:

- Fixed pool of **4 POD voice slots** per drum voice: `{cursor/phase,
  startOffset, gain}`. Round-robin steal-oldest with a ~2 ms steal fade.
  Regression test: two same-voice triggers inside one 4096-sample block produce
  two audible onsets.
- **Synth**: envelope amplitude × `velocity01^1.5`; excitation scaled
  `0.7 + 0.3 * vel`. Headroom fix folded in: per-voice gains rebalanced ~−6 dB,
  `s / (1 + |s|)` soft clip on the mix bus, plus a densest-step ≤ 1.0 test.
- **Sampler**: velocity is simply the per-slot gain.

Roadmap safety: the `onTrigger(..., velocity01)` plumbing lands **with velocity
hard-wired to 1.0 first**, so every existing audio test stays green mid-
migration; real velocities switch on with the `GrooveTable` step.

### 2.5 RT-safe regeneration / swap

There are two regimes, and the built-in case deliberately needs *no* machinery:

**Built-in styles (steady state, ships first).** The three styles are immutable
`constexpr` data. "Switching styles" is publishing one index:

```
message thread: APVTS listener (edge-detect, fires ONLY on change — A2 fix)
                    └─▶ pendingStyleIdx.store(i, std::memory_order_release)
audio thread:   at the step-0 bar wrap:
                    activeTable = &kStyles[pendingStyleIdx.load(acquire)];
```

No copy, no lock, no generation counter — switching is reading different const
data. This dissolves the confirmed A2 hazard (`processBlock` calling
`setStyle`/`setDensity` and rebuilding layers every block,
`PluginProcessor.cpp:137-138`).

**Future dynamic tables (user-edited grids, fills).** A preallocated, lock-free
**2-slot mailbox** in `AdaptiveDrummer`, dormant until editable grids ship:

```
producer (message thread / ThreadPool job):
    build GrooveTable into slots[free]           // off audio thread, may allocate
    pendingSlot.store(free, memory_order_release)
consumer (audio thread, at bar wrap only):
    if (int s = pendingSlot.exchange(-1, memory_order_acquire); s >= 0)
        activeTable = &slots[s];                 // pointer flip, no copy
```

Consuming at the bar wrap keeps structural changes musically aligned and
eliminates the try-lock-scope dropout class entirely. **No SpinLock anywhere
new.** The existing C1 sampler try-lock is *shrunk* to cover `mixVoices` only:
`forEachStepTrigger` reads only table + playhead and moves outside the lock into
a fixed trigger array (also fix the stale comment at `DrumSampler.h:65`).

**Groove seed / bar index.** `grooveSeed` is a user-visible, **non-automatable**
int in the state tree; `barIdx` increments at each bar wrap. Both feed the
stateless hash, guaranteeing deterministic bounces.

---

## 3. Follow another track

### 3.1 Sidechain bus topology — `PluginProcessor.cpp:46-66`

Re-introduce the sidechain as a **disabled-by-default auxiliary input bus**:

```cpp
BusesProperties()
    .withInput ("Input",     juce::AudioChannelSet::stereo(), true)
    .withOutput("Output",    juce::AudioChannelSet::stereo(), true)
    .withInput ("Sidechain", juce::AudioChannelSet::stereo(), false);
```

`isBusesLayoutSupported` keeps today's main-bus rule unchanged; the sidechain is
independently allowed to be **disabled, mono, or stereo**, and is never coupled
to the main layout. This is the strictness-10 hot zone: the bus change ships in
the same PR as a new `AdaptiveDrummerProcessorTest` (accepted/rejected layouts,
sine-guide-raises-intensity, state round-trip) and a full
`pluginval --strictness-level 10` run on both CI platforms.

### 3.2 Guide selection and fallback

Priority order, evaluated per block:

1. **Sidechain enabled and connected** → analyze the sidechain (true
   follow-another-track).
2. **Main input enabled** → analyze the main input *before* it is overwritten —
   today's "monitor mode" behavior is preserved as the fallback.
3. **Neither** (e.g. Standalone with no device input routed) →
   `processSilence`: Follow degrades gracefully to the manual puck; the followed
   atomics simply stop being written and the last manual/param values hold.

Critical correctness note: the `buffer.clear()` at `PluginProcessor.cpp:104`
becomes a per-channel **clear of main-out channels only** — clearing the whole
buffer would destroy the sidechain data that JUCE may alias into the same
`AudioBuffer`.

### 3.3 Per-DAW routing (ships as README section + UI tooltip)

| Host | How the user routes a guide track into the sidechain |
|---|---|
| REAPER | Send guide → plugin track, set to channels 3/4; plugin pin matrix maps 3/4 → Sidechain |
| Ableton Live | Device's "Audio From" chooser (sidechain section) → pick guide track |
| Cubase / Studio One | Activate the **sidechain button** in the plugin header, then send/route the guide to it |
| FL Studio | Guide mixer track → "Sidechain to this track"; in the wrapper, Processing tab → set sidechain input 1 |
| Bitwig | Sidechain source picker in the plugin header |
| Standalone | No "other track" exists — the audio **device input** is the guide (fallback path 2) |

### 3.4 Analysis → axis mapping — `EnergyAnalyzer`

- **intensity ← energy, directly.** `energyAnalyzer.getEnergy()` (existing
  RMS → dB → 30/300 ms attack/release envelope) feeds intensity **unquantized**
  — this retires the old energy→3-step-density collapse. A ~200 ms one-pole on
  top stops per-hit velocity flutter.
- **complexity ← onset rate.** A rising-edge counter on the existing
  `envelopeLevel` (O(1) per block, no FFT) is normalized to onsets-per-bar over
  a 1–2 bar window. The input passes through a **±0.08 dead-band** (the old
  `nextDensity` hysteresis, generalized) and is **bar-latched** — structural
  busy-ness changes only at bar wraps, which is the chatter protection the
  manual path doesn't need.
- Followed values are written to atomics and exposed to the editor read-only
  (extend the `getCurrentDensity()` pattern in `PluginProcessor.h`). They are
  **never written back into APVTS parameters** — Follow modulates, it does not
  move the user's knobs or fight host automation.

---

## 4. Parameter & state migration — `density` → `intensity` + `complexity`

`PluginProcessor.cpp:9-41, 164-183`:

1. **Add** `ParameterID{"intensity", 2}` and `ParameterID{"complexity", 2}` —
   Float 0–1, defaults **(0.55, 0.55)**, i.e. mask-equivalent to today's
   Medium. The version hint **must be 2**: JUCE version hints exist precisely
   for parameters added after v1, and some hosts key parameter identity on
   them.
2. **Keep `"density"` registered** (same ID, same type) as a **deprecated
   macro**: a message-thread APVTS listener edge-detects writes to it and maps
   `Sparse → (0.35, 0.20)`, `Medium → (0.55, 0.55)`, `Full → (0.85, 1.00)` via
   `setValueNotifyingHost`. Last-writer-wins between density automation and
   direct intensity/complexity moves; the listener never runs on the audio
   thread; the control is hidden in the UI. Removal is deferred to a major
   version so existing host automation lanes keep working.
3. **State versioning.** `getStateInformation` sets
   `state.setProperty("stateVersion", 2, nullptr)`. In `setStateInformation`,
   an absent property means v1: after `replaceState`, derive
   intensity/complexity from the restored density via the macro mapping and set
   a default `grooveSeed`. By mask-equivalence (§2.1), old sessions load
   **sounding identical**. Round-trip tests plus a hand-built v1 blob test land
   in the same commit.
4. `DrumPattern::setDensity` survives transitionally as
   `setComplexity(mapLegacy(d))` and is deleted, with its tests, in one commit
   once the UI reads the new axes.

---

## 5. Audio-first; MIDI-out is a leaf

**Decision: audio-first. MIDI-out is a small optional leaf at the end, not a
phase.** Reasons:

- Every tested asset in the repo — both engines, the C1 swap, all five test
  suites, the pluginval gate — guards the **audio** path. MIDI-first would move
  development onto the untested path.
- Standalone has **no MIDI destination**; MIDI-first strands one of the two
  shipping formats.
- Flipping `producesMidi()` / plugin category mid-roadmap risks
  strictness-10 and host-cache churn.
- The B4/B2 timing bugs corrupt a MIDI stream exactly as badly as an audio
  stream — MIDI-first buys no schedule.

After the engine-hygiene step, the engine's product is exactly
`(voice, sampleOffset, velocity01)`. A later **"MIDI Out" bool** maps those
triggers to GM notes (Kick 36, Snare 38, Closed HH 42, Crash 49, Ride 51,
Tom 45) into the `MidiBuffer` that `processBlock` already receives and ignores
(`PluginProcessor.cpp:89`). It is a leaf feature, delegable to a cheaper model.

---

## 6. Threading & RT-safety summary

| Value | Written by | Read by audio thread as | Applied |
|---|---|---|---|
| intensity | APVTS / Follow atomic | relaxed atomic → `SmoothedValue` (~50 ms) | immediately (non-structural) |
| complexity (manual) | APVTS | relaxed atomic, sampled per **step boundary** | at each step |
| complexity (followed) | analyzer (audio thread) | atomic, dead-band ±0.08 | **bar-latched** |
| style | APVTS listener (edge-detect, message thread) | `std::atomic<int> pendingStyleIdx` | bar wrap → const-table pointer flip |
| grooveSeed | state tree (non-automatable) | plain int + `barIdx` counter | every hash call |
| dynamic tables (future) | message thread / `ThreadPool` | 2-slot lock-free mailbox | bar wrap → pointer flip |

Hard rules preserved everywhere: no allocation, no locks, no file I/O on the
audio thread; sampler try-lock scope shrunk to `mixVoices` only; structural
changes land at bar wraps; per-hit changes land at step boundaries.

---

## 7. Roadmap (each step must be green: Linux+Windows build, CTest, pluginval 10)

1. **Guard tests** (pure additions): `DrumStepClockTest.cpp`,
   `AdaptiveDrummerProcessorTest` skeleton, C1 concurrency stress test.
2. **Clock rewrite**: exact-double boundaries, fire-once cursor, tolerance ppq
   re-lock, `hasPpq`/`isPlaying` split, time-signature pass-through, swing hook.
3. **Engine hygiene**: 4-slot pools + `onTrigger(..., vel)` **with velocity
   fixed at 1.0** (all existing audio tests still pass), headroom/soft-clip,
   stop-tail + `getTailLengthSeconds`, sampler try-lock shrunk, stale-comment
   fix.
4. **GrooveTable**: const style tables, mask-equivalent seeding, seeded-hash
   probability + real velocities live, restructured pattern tests, A2
   edge-detect + atomic style index + bar-latched style. Legacy density drives
   it via `mapLegacy`.
5. **Params/state**: intensity/complexity (version hint 2), stateVersion=2,
   density macro, grooveSeed, migration tests.
6. **Sidechain + Follow 2-D**: bus topology, gated guide selection,
   main-out-only clear, onset-rate feature, energy→intensity /
   activity→complexity with dead-band + bar latch; pluginval on both platforms
   gates this PR.
7. **Polish**: sampler failure-path/resampling, async kit loading, Jazz
   re-author with swing + authored velocities, `TableExchange` mailbox
   (dormant until editable grids).
8. **Optional leaves**: MIDI-out toggle, synth/sampler toggle reset.

---

## 8. Key file anchors

| File | Role in this design |
|---|---|
| `src/drummer/DrumPattern.{h,cpp}` | `StepCell`/`GrooveTable`, const style tables, firing rule |
| `src/drummer/DrumStepClock.h` | exact-double clock, swing, `onTrigger(voice, offset, vel01)` |
| `src/drummer/AdaptiveDrummer.{h,cpp}` | active-table pointer, bar wrap, style atomic, future mailbox |
| `src/drummer/DrumSynth.cpp` / `DrumSampler.cpp` | 4-slot pools, velocity consumption, headroom |
| `src/drummer/EnergyAnalyzer.{h,cpp}` | energy→intensity, onset-rate→complexity |
| `src/PluginProcessor.{h,cpp}` | buses, guide priority, params v2, state migration, density macro |
| `tests/` | clock, processor, concurrency, pattern-invariant, migration tests |

---

## 9. Resolutions to the six open questions

The questions are quoted from [`FABLE_HANDOFF.md`](FABLE_HANDOFF.md) §6.

**Q1 — Which 2-D model (A/B/C or hybrid) best preserves authored musical
quality while being continuous, and how is it tuned/authored?**
A **C/B hybrid**: a layered, threshold-gated table (Option C's authored base +
embellishments, generalized to a per-cell `complexityThreshold`) with Option B's
probabilistic element confined to a per-cell `probability` byte driven by a
stateless seeded hash. Authoring = writing one `GrooveTable` per style
(§2.1): the deterministic backbone is hand-authored exactly like today's
layers (probability 255), embellishments get graded thresholds, and ornaments
get sub-255 probabilities and `velRandRange`. Continuity comes from the
threshold gate (complexity) and the `velLow→velHigh` lerp (intensity), not from
interpolating between whole patterns — so every audible bar is an authored
pattern, never a blend artifact. Tuning invariants are encoded as tests
(backbone placement, monotonic hit count, superset property).

**Q2 — Audio-first or MIDI-out-first?**
**Audio-first**; MIDI-out is an optional leaf (§5). All tested/CI-guarded
assets are on the audio path, Standalone has no MIDI destination, flipping
`producesMidi()` mid-roadmap risks strictness-10 churn, and the open timing
bugs would corrupt MIDI identically — MIDI-first buys nothing. Once the engine
emits `(voice, sampleOffset, velocity01)`, a "MIDI Out" bool that writes GM
notes into the already-received `MidiBuffer` is a trivial, delegable feature.

**Q3 — Sidechain design: bus topology, per-DAW routing, which features drive
which axis, and the no-sidechain fallback?**
Topology: an optional, disabled-by-default stereo "Sidechain" input bus,
layout-validated independently of the main buses (§3.1). Routing recipes per
host ship in the README and a UI tooltip (§3.3). Mapping: **energy →
intensity** (unquantized, ~200 ms smoothing) and **onset rate → complexity**
(dead-band ±0.08, bar-latched) (§3.4). Fallback chain: sidechain → own main
input (today's monitor mode) → silence/manual puck (§3.2). Guard rail: never
`clear()` the whole buffer — main-out channels only.

**Q4 — Where does dynamic pattern generation run, and how is the swap made
RT-safe?**
For the shipping feature set there is **no generation at all**: styles are
immutable `constexpr` tables and switching is an atomic index applied at the
bar wrap — the A2 rebuild-per-block hazard is dissolved rather than mitigated
(§2.5). Per-hit variation is computed inline on the audio thread from the
stateless hash (no state, no allocation). When user-editable grids/fills
arrive, tables are built on the **message thread / a `ThreadPool` job** and
handed over via a preallocated 2-slot lock-free mailbox consumed at the bar
wrap — a strict improvement on reusing the C1 try-lock idiom, because it
eliminates the try-lock-dropout class entirely.

**Q5 — Parameter/state migration: `density` → `intensity` + `complexity`
without breaking saved sessions/automation?**
Add the two new params with **`ParameterID` version hint 2**; **keep `density`
registered** as a hidden deprecated macro that fans out to the new params on the
message thread (so existing automation lanes still work, last-writer-wins);
stamp `stateVersion = 2` and migrate absent-version (v1) states by deriving the
new axes from the restored density (§4). Because table seeding is
mask-equivalent to the legacy layers (§2.1), migrated sessions load
bit-identically in musical content. Removal of `density` waits for a major
version.

**Q6 — Velocity/accent data model and how both engines consume it?**
Two authored velocity columns per cell — `velLow`/`velHigh`, the velocities at
intensity 0 and 1 — plus `velRandRange` humanization from the seeded hash;
runtime velocity is `lerp(velLow, velHigh, intensity^0.7)` (§2.1–2.2). This
re-voices rather than merely scales (ghosts stay ghosts), and subsumes any
accent-compression formula in the authoring data. Both engines receive it
through the unified `onTrigger(voice, offsetInBlock, velocity01)` callback:
the **synth** maps it to envelope amplitude (`vel^1.5`) and excitation
(`0.7 + 0.3·vel`); the **sampler** uses it as the per-slot gain. Both gain
4-slot polyphonic voice pools in the same pass, fixing B2 so same-voice
retriggers overlap instead of dropping.
