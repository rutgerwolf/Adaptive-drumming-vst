# ROADMAP_2D — Dependency-ordered implementation plan for the 2-D (intensity × complexity) engine

This roadmap turns the architecture in `docs/ARCHITECTURE_2D_FOLLOW.md` (Candidate B base + Candidate A grafts: seeded-hash probability, groove seed, lock-free table mailbox) into a sequence of steps a model can execute one at a time. **Every step must leave `main` green.** Steps are ordered so that no step depends on a later one, and every confirmed review finding is fixed at an explicitly marked step.

---

## 0. Ground rules (read before every step)

### Definition of "green" (gate after EVERY step)

1. `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DADAPTIVE_DRUMMER_BUILD_TESTS=ON && cmake --build build` succeeds on **Linux and Windows** (CI: `.github/workflows/ci.yml`, both jobs).
2. `ctest --test-dir build --output-on-failure` passes (test target `AdaptiveDrummerTests`).
3. pluginval passes at **strictness 10** on both platforms (CI uses `--strictness-level 10 --random-seed 1 --validate-in-process` on the Release VST3). Any step touching `PluginProcessor.cpp` bus/param/state code must be checked against pluginval locally before merging.

### RT-safety rules (apply to all audio-thread code, i.e. anything reachable from `processBlock`)

- **Never** on the audio thread: heap allocation/free, `new`/`delete`, `std::vector` growth, blocking locks (`SpinLock::enter`), file I/O, logging, `juce::String` construction, `MessageManager` calls.
- Cross-thread values travel via `std::atomic<...>` (relaxed order unless a comment justifies acquire/release) or via the existing DrumSampler C1 try-lock idiom. The audio thread only ever *try*-locks.
- Structural changes (style, table pointer, followed complexity) apply at the **bar wrap (step 0)**; continuous changes (intensity, volume) apply immediately, smoothed.
- All new pattern data is POD, `const`/`constexpr` where possible, sized at compile time. No `juce::Random` on the audio thread — randomness is the stateless SplitMix64 hash only.

### Process rules

- One step = one PR/commit series; do not start step N+1 until step N is merged green.
- When a step changes behaviour a test asserts, **update the test in the same commit** — never delete an assertion without a replacement invariant.
- New test files must be added to `AdaptiveDrummerTests` sources in `CMakeLists.txt` (lines ~98–110).
- Sizes: **XS** < 50 LOC, **S** 50–150, **M** 150–400, **L** 400–800 (including tests).

### Key files

| Area | File |
|---|---|
| Pattern data model | `src/drummer/DrumPattern.h` (`layers[3][16]` at line 49), `src/drummer/DrumPattern.cpp` |
| Step clock | `src/drummer/DrumStepClock.h` |
| Conductor | `src/drummer/AdaptiveDrummer.h/.cpp` |
| Engines | `src/drummer/DrumSynth.cpp`, `src/drummer/DrumSampler.cpp` (C1 lock at `.cpp:67-73`, comment at `.h:65`) |
| Analyzer | `src/drummer/EnergyAnalyzer.h/.cpp` |
| Processor/params/buses/state | `src/PluginProcessor.h/.cpp` |
| Editor | `src/PluginEditor.h/.cpp` |
| Tests | `tests/*.cpp`, registered in `CMakeLists.txt` |

---

## Step overview (dependency order)

| # | Step | Fixes (confirmed findings) | Size |
|---|---|---|---|
| 1 | Guard tests: clock free-run + C1 concurrency stress | test-debt: "clock has zero direct tests", "no C1 concurrency test" | M |
| 2 | Processor-level test scaffolding | test-debt: "no processor-level test below pluginval" | M |
| 3 | Clock rewrite: exact-double boundaries + swing hook | phantom 17th step (low); prerequisite for B4 | M |
| 4 | Host sync: tolerance ppq re-lock, hasPpq split, time sig | **B4×B3 double/dropped triggers (high)**, playing-without-ppq (med), 4/4 hardcode (low) | M |
| 5 | Engine polyphony + velocity plumbing (vel = 1.0) | **B2 same-block trigger loss / retrigger click (high)** | M |
| 6 | Synth headroom: gain rebalance + soft clip | synth sum > full scale (med) | S |
| 7 | Sampler lock scope shrink + comment fix | try-lock drops a block's triggers (low) | S |
| 8 | Stop/toggle hygiene: tails, `getTailLengthSeconds`, engine-switch reset | stop hard-reset click (low), toggle stale tails (low) | S |
| 9 | DrumPatternTest → structural invariants | prerequisite for D1 (called out in A2 finding) | S |
| 10 | Edge-detect parameter pushes in `processBlock` | **A2 per-block `setStyle` rebuild (med)** | S |
| 11 | GrooveTable data model + seeded-hash firing + bar-latched style | **D1 no velocity data (high)**, mid-bar structural change (low) | L |
| 12 | Params & state migration: intensity/complexity, stateVersion=2, density macro, grooveSeed | no state-version marker (low) | M |
| 13 | Sidechain bus + gated guide selection | **no sidechain bus (high)**, analyse-disabled-input (low) | M |
| 14 | Follow 2-D mapping: energy→intensity, onset-rate→complexity | follow quantized through 3-step density (low) | M |
| 15 | Sampler failure-path integrity | loadSamples corrupts kit state (med) | S |
| 16 | Sampler resampling at load | pitched/off-tempo kits (med) | S |
| 17 | Async kit loading | blocking I/O in prepare/setState (low) | M |
| 18 | Jazz re-author: swing + velocities | Jazz genre-wrong (low) | S |
| 19 | `TableExchange` mailbox (dormant) + optional MIDI-out leaf | — (future-proofing) | M |

---

## Step 1 — Guard tests: clock free-run + C1 concurrency stress

**Goal.** Land regression nets *before* touching timing or lock code. Both tests must pass against the **current** implementation (they test free-run behaviour, which is correct today; the ppq re-lock bug lives in `AdaptiveDrummer` and gets its failing-today test in Step 4, in the same commit as the fix).

**Files.** New `tests/DrumStepClockTest.cpp`; extend `tests/DrumSamplerTest.cpp`; register new file in `CMakeLists.txt` test sources.

**What to build.**
- `DrumStepClockTest`: drive `forEachStepTrigger` with a counting lambda over a full free-running bar at 130 BPM / 44.1 kHz (non-integral bar: 81 415.38 samples) and 120 BPM / 48 kHz (integral). Sweep block sizes {1, 64, 480, 4096}, advancing `playheadSample` by exactly `numSamples` each call. Assert: exactly 16 step firings per bar; each step index fires exactly once; no firing at step index 16; triggers land at the expected voices for Rock/Medium.
- C1 stress: one thread loops `mixVoices`-equivalent processing (call `DrumSampler::processBlock` via its public API) for ~1 s while another thread alternates `loadSamples` between two temp kits (write two tiny WAVs of different lengths to the scratch dir with `juce::WavAudioFormat`). Assert no crash, all output samples finite, and a post-swap block renders the new kit's content.

**RT-safety.** Tests only; no production code changes. The stress test *documents* the invariant that `loadSamples` allocates outside the lock and the audio side never blocks.

**Test to add.** The step *is* tests. Gate: both new tests pass on current `main`.

**Risk.** Low. Main hazard is writing a clock test that encodes the phantom-17th bug as correct — assert "no trigger at index 16" via voice callbacks (true today because `getStep(16)` returns 0), not "no 17th boundary scan".

**Size.** M.

---

## Step 2 — Processor-level test scaffolding

**Goal.** Get `AdaptiveDrummerProcessor` instantiable inside `AdaptiveDrummerTests` so later bus/state/follow steps have a CTest net below pluginval.

**Files.** New `tests/AdaptiveDrummerProcessorTest.cpp`; `CMakeLists.txt` (add `src/PluginProcessor.cpp` and `src/PluginEditor.cpp` to test sources; link the additional modules the processor needs: `juce_audio_processors`, `juce_audio_utils`, `juce_audio_devices`, `juce_data_structures`, `juce_events`, `juce_graphics`, `juce_gui_basics`, `juce_gui_extra`; keep `JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0`).

**What to build.**
- A `juce::ScopedJuceInitialiser_GUI` in the test fixture (the APVTS/editor machinery needs a MessageManager). Never call `createEditor()` in tests — headless CI. If Linux CI needs a display for GUI-module init, run ctest under `xvfb-run` (xvfb is already installed by `ci.yml`).
- Tests against **current** behaviour: (a) accepted layouts — stereo/stereo, mono/mono, disabled-in/stereo-out; rejected — mono-in/stereo-out, quad; (b) `prepareToPlay` + 100 blocks of `processBlock` with `play=1`, assert non-silent finite output; (c) state round-trip: set params, `getStateInformation` → fresh processor → `setStateInformation`, assert param values and `samplesPath` property survive.

**RT-safety.** Tests only. Note in the test file: `processBlock` is called from the test thread; do not add message-thread calls inside it.

**Test to add.** The step is tests. Gate: passes on current `main`, both platforms.

**Risk.** Medium — CMake/link friction (missing `JucePlugin_*` macros are *not* needed since `juce_audio_plugin_client` is excluded; if a stray macro is referenced, define it via `target_compile_definitions` on the test target only). If Windows CI chokes on GUI modules in the console app, wrap processor tests in `#if !JUCE_WINDOWS` **only as a last resort** and open an issue; the goal is both platforms.

**Size.** M.

---

## Step 3 — Clock rewrite: exact-double boundaries + swing hook

**Goal.** Replace per-sample modulo scanning in `forEachStepTrigger` with exact-double step boundaries walked by a cursor. Kills the phantom 17th boundary and the up-to-15-sample grid compression; creates the swing seam.

**Files.** `src/drummer/DrumStepClock.h`; `src/drummer/DrumPattern.h/.cpp` (`getLengthInSamples` returns `double`, exact: `4.0 * 60.0 / bpm * sampleRate * ...`); `src/drummer/AdaptiveDrummer.cpp` (adapt to `double` length — keep `playheadSample` as `int` for now, cast at call sites; the full double playhead lands in Step 4); `tests/DrumStepClockTest.cpp`.

**What to build.**
- `patternLenExact` as `double`; step boundary k at `llround(k * patternLenExact / 16.0)` for k = 0..15; walk a `nextStepSample` cursor forward instead of testing every sample (`for` over boundaries falling in `[playhead, playhead + numSamples)`, handling wrap). No modulo per sample.
- Swing parameter (`double swing01`, default 0.0, plumbed through the function signature): odd step indices get `+ swing01 * stepLenExact * (2.0/3.0 max)` **before** rounding. Nothing sets it non-zero yet.
- Keep the callback as `onTrigger(voice, offset)` — arity changes in Step 5.

**RT-safety.** Pure function, integer/double arithmetic only, no state, no allocation. Boundary computation is O(steps-in-block), cheaper than today's O(numSamples).

**Test to add.** Extend `DrumStepClockTest`: at 130 BPM/44.1 kHz assert boundary sample positions equal `llround(k * 81415.3846.. / 16)` exactly; assert exactly-once across a block edge that splits a boundary; add a swing case (swing 0.5 → odd boundaries delayed by the expected sample count, even boundaries unmoved); re-run all Step 1 cases unchanged.

**Risk.** Medium — off-by-one at the bar wrap (boundary 0 of the next bar vs boundary 16 of this one). The exactly-once assertions from Step 1 are the guard; do not weaken them.

**Size.** M.

---

## Step 4 — Host sync: tolerance ppq re-lock, hasPpq/hostPlaying split, time signature

**Goal.** Fix the confirmed **high** B4×B3 finding (`AdaptiveDrummer.cpp:71`): the per-block ppq re-derivation against truncated lengths double-fires or drops steps at non-integral tempos. Also fix playing-without-ppq silence (`PluginProcessor.cpp:123-127`) and hardcoded 4/4.

**Files.** `src/drummer/AdaptiveDrummer.h/.cpp`; `src/PluginProcessor.cpp` (transport block, lines ~110–135); `tests/AdaptiveDrummerTest.cpp`.

**What to build.**
- `playheadSample` becomes `double`, free-running (`+= numSamples` each block). Each block, if ppq is available, compute `ppqDerived` in double precision (exact pattern length from Step 3); **only re-lock** (`playheadSample = ppqDerived`) when `|ppqDerived − playheadSample| > tolerance` (~2 ms worth of samples). Otherwise free-run.
- `setHostTimeline(bool isPlaying, bool hasPpq, double ppq, double beatsPerBar)` — split the flags. In `PluginProcessor.cpp`, hoist `hostPlaying = pos->getIsPlaying()` **out** of the `getPpqPosition()` branch; set `hasPpq` only when ppq is present; read `pos->getTimeSignature()` into `beatsPerBar` (default 4.0). Playing without ppq ⇒ free-run, not silence.
- Clamp host bpm once, in one place, before **both** pattern-length and ppq mapping (today `setBpm` clamps to [40, 240] but the ppq math sees the unclamped tempo — make them consistent; prefer widening the clamp for host-provided tempi).
- `ppqToPlayhead` returns `double`, honours `beatsPerBar`.

**RT-safety.** All double arithmetic on the audio thread; no allocation. The tolerance re-lock means a jittery host ppq no longer rewrites the playhead every block — strictly less work.

**Test to add.** In `AdaptiveDrummerTest`: (a) multi-block host-sync regression — 130 BPM/44.1 kHz, feed monotonically advancing ppq each block (block sizes 480 and 4096), count triggers per step over 8 bars, assert **each of 16 steps fires exactly once per bar** (this test fails on pre-step-4 code — land it in the same commit as the fix); (b) playing=true/hasPpq=false produces triggers; (c) 3/4 time signature keeps the downbeat aligned over 4 host bars; (d) transport loop-jump (ppq jumps back a bar) re-locks within one block.

**Risk.** Medium-high — this is the timing heart. Guard rails: Step 1/3 clock tests unchanged, plus the new exactly-once test. Watch the interaction with `drummer.reset()` on stop (unchanged until Step 8).

**Size.** M.

---

## Step 5 — Engine polyphony + velocity plumbing (velocity hard-wired to 1.0)

**Goal.** Fix confirmed **high** B2 (`DrumSampler.cpp:125-129`, `DrumSynth.cpp:24-32`): single cursor per voice drops the first of two same-block triggers and clicks on retrigger. Simultaneously change the trigger signature to `onTrigger(int voice, int offsetInBlock, float velocity01)` with the clock passing **1.0f** everywhere, so every existing audio test still passes and Step 11 only has to change the *value*.

**Files.** `src/drummer/DrumStepClock.h` (callback arity, still passes 1.0f); `src/drummer/DrumSynth.h/.cpp`; `src/drummer/DrumSampler.h/.cpp`; `src/drummer/AdaptiveDrummer.cpp` (lambda arity); `tests/DrumSynthTest.cpp`, `tests/DrumSamplerTest.cpp`.

**What to build.**
- Both engines: fixed pool of **4 POD slots per voice** — sampler slot `{int playPos; int startOffset; float gain; bool active;}`, synth slot `{double t/phase state…; float gain; bool active;}`. Trigger claims a free slot; if none, round-robin steal the **oldest** with a ~2 ms linear steal-fade (apply fade-out to the stolen slot's remaining contribution within this block, or ramp the new slot in — pick one, document it).
- Velocity: synth amplitude × `velocity01^1.5`, excitation scaled `0.7 + 0.3 * vel`; sampler per-slot `gain = velocity01`. With vel pinned at 1.0 output is bit-comparable to a single-trigger case.
- While in `DrumSynth.cpp`: remove/replace the unseeded `juce::Random` usage if present in the trigger path (deterministic noise seed per trigger is fine as an interim; the hash-based scheme arrives in Step 11).

**RT-safety.** Slot pools are fixed-size members — zero heap on trigger. Steal-fade is per-sample multiply. No locks added.

**Test to add.** Regression in both engine tests: two triggers of the same voice at offsets 0 and 3000 within one 4096-sample block ⇒ two distinct onsets (detect two rising-energy edges). Retrigger of a long voice (crash) mid-decay ⇒ no sample-to-sample discontinuity above a threshold (assert max |delta| between adjacent samples < ~0.5). All pre-existing tests must pass **unmodified** except for lambda-arity mechanical updates.

**Risk.** Medium — the synth's per-voice DSP state must be fully moved into the slot struct; missing one field (e.g. a filter state) gives subtle artifacts, not test failures. Add an assertion test that a single trigger through the new pool is sample-identical (or within 1e-6) to the old single-voice render for one voice, if feasible.

**Size.** M.

---

## Step 6 — Synth headroom: gain rebalance + soft clip

**Goal.** Fix confirmed **med** finding: Rock/Electronic Full step 0 (Crash|Kick|HiHat) peaks ~1.9 after default volume. Rebalance per-voice gains ~−6 dB and add a cheap soft clip on the synth mix bus.

**Files.** `src/drummer/DrumSynth.cpp` (per-voice gain table, mix loop lines ~51–65); `tests/DrumSynthTest.cpp`.

**What to build.** Scale voice gains down ~−6 dB (retune by ear ratio, keep relative balance); apply `s / (1 + |s|)` (or `std::tanh`) per sample on the summed synth output before it leaves `DrumSynth`. Do **not** clip the sampler bus (samples are normalized; revisit if Step 16 changes that).

**RT-safety.** One divide/abs per sample on the audio thread — trivial. No state.

**Test to add.** For each style at Full/max: render the densest step with all coincident voices at velocity 1.0, assert `max |sample| <= 1.0`. Keep an existing-tests sanity pass — amplitude-threshold assertions in `DrumSynthTest` may need their expected levels scaled in the same commit.

**Risk.** Low. Only hazard is an existing test asserting absolute output levels — adjust thresholds, don't delete assertions.

**Size.** S.

---

## Step 7 — Sampler lock scope shrink + comment fix

**Goal.** Fix confirmed **low** finding at `DrumSampler.cpp:67-73`: the try-lock currently wraps `forEachStepTrigger` + `mixVoices`, so a swap-coincident block permanently loses its triggers. Also fix the stale comment at `DrumSampler.h:65` claiming the lock is "held only for the cheap swap".

**Files.** `src/drummer/DrumSampler.h/.cpp`.

**What to build.** Run `forEachStepTrigger` **outside** the lock (it reads only pattern + playhead) into a small fixed array (`struct Trig { int voice, offset; float vel; }; Trig trigs[64]; int n;` — 64 covers 16 steps × up to 4 stacked voices per block worst case; clamp and drop beyond, with a comment). Take the try-lock only around slot-claiming + `mixVoices`. If the lock is missed, the triggers for that block are dropped but the playhead was still advanced by the caller — same as today, now with a far smaller window; note this in the comment. Update `DrumSampler.h:65` to describe the real critical section.

**RT-safety.** Strictly shrinks audio-thread lock hold time; fixed array on the stack, no allocation.

**Test to add.** Re-run the Step 1 C1 stress test (it now also exercises the narrowed window). Add a single-threaded test: triggers computed while lock available equal triggers computed under contention-free path (behavioural no-op check).

**Risk.** Low.

**Size.** S.

---

## Step 8 — Stop/toggle hygiene: tails, `getTailLengthSeconds`, engine-switch reset

**Goal.** Fix two confirmed **low** findings: (a) transport stop calls `drummer.reset()` every stopped block (`PluginProcessor.cpp:155`), truncating tails with a click, while `getTailLengthSeconds()` returns 0; (b) toggling Synth↔Samples (`AdaptiveDrummer.h:33`) freezes the inactive engine's voices, which resume audibly on toggle-back.

**Files.** `src/PluginProcessor.cpp/.h`; `src/drummer/AdaptiveDrummer.h/.cpp`; `tests/AdaptiveDrummerTest.cpp`, `tests/AdaptiveDrummerProcessorTest.cpp`.

**What to build.**
- Edge-detect playing true→false in the processor (plain `bool wasPlaying` member, audio thread only): on the edge, rewind the drummer playhead once; while stopped, keep calling the drummer's render path with **triggers suppressed** so active slots decay to silence naturally. Add `AdaptiveDrummer::renderTails` or a `bool triggersEnabled` flag — pick the smaller diff.
- `getTailLengthSeconds()` returns `1.5` (covers the ~1.2 s crash decay).
- `setUseSynth`: on flag change, `reset()` the engine being switched **away from** (both resets are plain member writes — RT-safe).

**RT-safety.** Edge-detect is a bool compare; resets are field writes. No allocation.

**Test to add.** (a) Trigger a crash, stop the transport mid-decay ⇒ output continues decaying, no adjacent-sample jump above threshold, eventually silent. (b) Toggle source mid-decay, toggle back ⇒ silence until the next step boundary (no resumed fragment). (c) Processor test asserts `getTailLengthSeconds() > 0`.

**Risk.** Low-medium — pluginval probes tail behaviour and stop/start cycles; run pluginval locally.

**Size.** S.

---

## Step 9 — DrumPatternTest → structural invariants

**Goal.** Convert raw-mask assertions in `tests/DrumPatternTest.cpp` into structural invariants **before** the D1 model change, so Step 11 can re-implement the model without rewriting the test's meaning. Explicitly called for by the A2 finding.

**Files.** `tests/DrumPatternTest.cpp` only.

**What to build.** Replace exact-bitmask expectations with invariants that are true today **and** must stay true after GrooveTable: kick present on steps 0 and 8 and snare on 4 and 12 for Rock/Medium; hit-count monotonically non-decreasing Sparse→Medium→Full for every style; Full is a superset of Sparse (per voice/step); crash appears only on step 0; every style/density has ≥1 kick. Keep `Voice` enum and `kSteps` assertions.

**RT-safety.** Tests only.

**Test to add.** The step is a test refactor; gate is green on current `main`.

**Risk.** Low. Do not weaken: each deleted mask assertion must map to a named invariant.

**Size.** S.

---

## Step 10 — Edge-detect parameter pushes in `processBlock`

**Goal.** Fix confirmed **med** A2: `processBlock` unconditionally calls `drummer.setStyle` (and `setDensity`, `setUseSynth`) every block (`PluginProcessor.cpp:137-148`), rebuilding all pattern layers on the audio thread each callback.

**Files.** `src/PluginProcessor.cpp/.h`.

**What to build.** Plain audio-thread members `lastStyle`, `lastDensity`, `lastSource` (ints, initialised to −1); call the drummer setters only when the raw parameter value changed. Note in a comment: this is transitional — Step 11 replaces the style path with an atomic index + bar latch, and Step 12 replaces density.

**RT-safety.** Removes per-block memset/re-author work from the audio thread. Compare-and-branch only.

**Test to add.** In `AdaptiveDrummerProcessorTest`: instrument via observable behaviour — process 100 blocks with constant params, then flip style once, assert output changes and (via a counter you temporarily expose or a friend-test hook on `DrumPattern` rebuild count if trivial) the rebuild happened once, not 100×. If counting is intrusive, a behavioural test (style flip mid-run works; constant params produce bit-identical repeated bars) is acceptable.

**Risk.** Low. Watch initial-value edge: first block must push all values (hence −1 sentinels).

**Size.** S.

---

## Step 11 — GrooveTable: data model, seeded-hash firing, live velocity, bar-latched style

**Goal.** The core D1 fix (**high**): replace `uint8_t layers[3][kSteps]` (`DrumPattern.h:49`) with the layered/interpolated `GrooveTable`, wire the firing rule and velocity into the engines (velocity value goes live — plumbing already exists from Step 5), and make style changes bar-latched via an atomic index (fixes the mid-bar structural change finding). Legacy density keeps driving everything through `mapLegacy` until Step 12.

**Files.** `src/drummer/DrumPattern.h/.cpp` (bulk); new `src/drummer/GrooveHash.h` (SplitMix64 + `hash01`/`hashBipolar`); `src/drummer/AdaptiveDrummer.h/.cpp`; `src/drummer/DrumStepClock.h` (consume per-style swing; compute per-step fire/velocity); `src/PluginProcessor.cpp` (style push becomes atomic-index write); `tests/DrumPatternTest.cpp`, `tests/AdaptiveDrummerTest.cpp`.

**What to build.**
- POD model exactly as specified in the architecture doc:
  ```cpp
  struct StepCell  { uint8_t velLow, velHigh, complexityThreshold, probability; int8_t velRandRange; uint8_t flags; };
  struct GrooveTable { StepCell cells[6][DrumPattern::kSteps]; uint8_t swing; uint32_t styleSalt; };
  ```
  Three `const` file-scope tables in `DrumPattern.cpp`, authored by evolving `buildRock/buildJazz/buildElectronic` into table authors. **Mask-equivalent seeding**: Sparse hits → threshold 0, Medium-added → ~100, Full-added → ~190, all `probability=255`, `velRandRange=0`; legacy density maps via `mapLegacy`: Sparse→complexity 0.2, Medium→0.55, Full→1.0 (intensity analogously 0.35/0.55/0.85). New ghost/ornament cells (threshold 200–240, probability <255, velRandRange ±8–15) may be authored for Rock/Electronic now or deferred to Step 18 — deferring is fine.
- Firing rule at each step boundary (integer/float only):
  ```cpp
  fires = cell.velHigh > 0 && complexity255 >= cell.complexityThreshold
       && (cell.probability == 255
           || hash01(seed ^ table.styleSalt, barIdx, voice, step) * 255.0f
                < cell.probability * probScale(complexity));
  vel01 = clamp01(lerp(velLow, velHigh, powf(intensity, 0.7f)) / 127.0f
       + hashBipolar(seed ^ table.styleSalt, barIdx, voice, step, 1) * cell.velRandRange / 127.0f);
  ```
  `hash01` is stateless SplitMix64 over packed inputs — zero RNG state, bit-exact realtime == offline bounce.
- `AdaptiveDrummer`: `setIntensity(float)` / `setComplexity(float)`; intensity read per block through a `juce::SmoothedValue` (~50 ms, same idiom as `volumeSmoothed` at `PluginProcessor.cpp:75`); complexity sampled once per **step boundary** into a local (`complexity255`). `barIdx` (uint32) increments at bar wrap. `seed` is a plain member with a fixed default constant (becomes user state in Step 12).
- Style: `std::atomic<int> pendingStyleIdx` written from `processBlock`'s edge-detect (Step 10 seam); applied on the audio thread **only at the step-0 bar wrap** by re-pointing `const GrooveTable* activeTable`. No copy, no lock, no generation counter.
- Swing: clock consumes `activeTable->swing` (Rock/Electronic = 0; Jazz stays 0 until Step 18).
- Keep class name `DrumPattern`, `Voice` enum, `kSteps`. `setDensity(d)` survives as `setComplexity/setIntensity(mapLegacy(d))` so the processor code and old tests keep working. `getStep()` may remain as a mask view derived from the table at the legacy density points (used by tests), or tests move to the new query API — prefer the latter, in this commit.

**RT-safety.** The three tables are immutable `const` data — a style change is one atomic int store (message-thread side is just the parameter read in `processBlock`; the *apply* is an audio-thread pointer repoint at bar wrap). Firing rule is branch+hash arithmetic per (voice,step) boundary — no allocation, no locks, no `juce::Random`. `powf` per trigger is acceptable (≤ 96 calls/bar); optionally precompute per block.

**Test to add.** (in the same commit)
- Structural invariants from Step 9 re-pointed at the new model and passing: kick 0/8 + snare 4/12 at Rock/complexity 0.55; hit-count monotonic in complexity (sweep 0→1 in 0.1 steps); complexity-1.0 fires a superset of complexity-0.2 for probability-255 cells; crash gated to step 0.
- Mask-equivalence: for each style, table hits at `mapLegacy(density)` with probability-255-only == the pre-step-11 layer masks (hardcode the old masks in the test as golden data).
- Determinism: same seed + barIdx ⇒ identical trigger/velocity sequence across two renders; different seed ⇒ different ornament pattern, identical probability-255 backbone.
- Bar-latch: style flip mid-bar takes effect exactly at the next step 0.
- Velocity live: intensity 0.2 vs 0.9 produce measurably different peak amplitudes for the same hit.

**Risk.** **High — largest step.** Mitigations: Steps 1–10 nets are all in place; the mask-equivalence golden test pins legacy behaviour; velocity plumbing already proven at 1.0. Do not combine with Step 12 — params/state stay untouched here.

**Size.** L.

---

## Step 12 — Params & state migration: intensity/complexity, stateVersion=2, density macro, grooveSeed

**Goal.** Expose the two axes as host parameters, version the state, keep `"density"` alive as a deprecated macro, and make the groove seed user state. Fixes the no-state-version finding.

**Files.** `src/PluginProcessor.cpp` (layout lines 9–41, state lines 164–183) / `.h`; `src/PluginEditor.h/.cpp` (two knobs replacing the density control; density hidden); `src/drummer/DrumPattern.h/.cpp` (delete transitional `setDensity` + `mapLegacy` call sites once unused); `tests/AdaptiveDrummerProcessorTest.cpp`, `tests/DrumPatternTest.cpp` (drop legacy-density-specific tests in the same commit).

**What to build.**
1. `ParameterID{"intensity", 2}` and `ParameterID{"complexity", 2}` — Float 0–1, defaults **(0.55, 0.55)** = mask-equivalent to today's Medium. **Version hint 2 is mandatory** (JUCE version hints exist for params added after v1; some hosts key on them).
2. Keep `"density"` registered (same ID/type — VST3 ID stability) as a hidden deprecated macro: a **message-thread** APVTS listener edge-detects density writes and maps Sparse→(0.35, 0.20), Medium→(0.55, 0.55), Full→(0.85, 1.00) via `setValueNotifyingHost`. Last-writer-wins. Never touched from the audio thread.
3. `grooveSeed`: non-automatable int property in the state tree (not an `AudioParameter`), default fixed constant; pushed to the drummer via atomic on change.
4. `getStateInformation`: `state.setProperty("stateVersion", 2, nullptr)` before serialising. `setStateInformation`: absent property ⇒ v1 ⇒ after `replaceState`, derive intensity/complexity from the restored density value and set the default grooveSeed — old sessions load sounding identical by mask-equivalence.
5. Audio thread reads intensity/complexity raw parameter values (atomics) directly, per block / per step boundary as wired in Step 11.

**RT-safety.** The density→axes mapping runs on the message thread only (APVTS listener). Audio thread reads two extra atomics. Seed change is an atomic int; it alters ornament hashing mid-bar, which is acceptable (non-structural).

**Test to add.** State round-trip with new params; **hand-built v1 blob test**: serialise an XML matching the v1 tree (no `stateVersion`, density=Full), feed to `setStateInformation`, assert intensity==0.85 / complexity==1.00 and identical first-bar trigger mask to legacy Full; density-macro test: set density param, assert both axes move; grooveSeed survives round-trip.

**Risk.** Medium — pluginval strictness 10 exercises parameter automation and state cycling hard; run it locally on both platforms. Editor changes are cosmetic but keep them minimal (two sliders bound via `SliderAttachment`).

**Size.** M.

---

## Step 13 — Sidechain bus + gated guide selection

**Goal.** Fix the confirmed **high** finding: no sidechain bus — the guide is the plugin's own input, which `processBlock` then clears (`PluginProcessor.cpp:56-66, 99-104`). Also fix the **low** finding: analysis reads the main buffer even when the input bus is disabled. **This is the pluginval strictness-10 hot zone — the pluginval run on both platforms gates this PR.**

**Files.** `src/PluginProcessor.cpp` (constructor buses, `isBusesLayoutSupported`, `processBlock` analyse/clear section); `tests/AdaptiveDrummerProcessorTest.cpp`; `README.md` (routing notes).

**What to build.**
- Constructor: `.withInput("Sidechain", juce::AudioChannelSet::stereo(), false)` (default-disabled).
- `isBusesLayoutSupported`: keep today's main-bus rule exactly; the sidechain bus is independently disabled/mono/stereo, **never coupled** to the main layout.
- Guide priority in `processBlock`: sidechain enabled with >0 channels → analyse `getBusBuffer(buffer, true, 1)`; else main input **enabled** with >0 channels → analyse main input before overwrite (today's monitor mode); else `processSilence`. The disabled-input case must never read the buffer (fixes stale-buffer self-follow).
- **Critical:** replace `buffer.clear()` at line 104 with clearing **only the main-out channels** (`mainOut.clear()` channel-by-channel) — never touch sidechain channel data; some hosts alias buffers.
- README: per-DAW routing notes (REAPER pin matrix 3/4; Live "Audio From"; Cubase/S1 header sidechain button; FL "Sidechain to this track" + wrapper input 1; Bitwig header picker; Standalone uses device input).

**RT-safety.** Bus queries and per-channel clears only; no allocation. `getBusBuffer` is a view, not a copy.

**Test to add.** Processor tests: accepted layouts now include {main stereo/stereo + SC disabled}, {+ SC mono}, {+ SC stereo}; rejected unchanged for main-bus violations; sidechain-fed sine raises `EnergyAnalyzer` energy while main input is silent; main input disabled + follow on ⇒ energy stays ~0; drums still render to main out with SC enabled. Then: **full pluginval strictness-10 on Linux and Windows before merge** (bus layout is its hardest probe).

**Risk.** High for CI-green — bus-layout code is the most common pluginval failure. Mitigation: land tests first in the PR, iterate against local pluginval, never modify the main-bus rule.

**Size.** M.

---

## Step 14 — Follow 2-D mapping: energy→intensity, onset-rate→complexity

**Goal.** Retire the density-collapse follow path (confirmed low: `PluginProcessor.cpp:144-148` quantizes continuous energy through the 3-step density). Followed values steer the two axes directly.

**Files.** `src/drummer/EnergyAnalyzer.h/.cpp`; `src/PluginProcessor.cpp/.h`; `src/PluginEditor.cpp` (readouts); `tests/EnergyAnalyzerTest.cpp`, `tests/AdaptiveDrummerProcessorTest.cpp`.

**What to build.**
- **intensity ← `energyAnalyzer.getEnergy()`** directly (existing 30/300 ms envelope, unquantized) through an additional ~200 ms one-pole to stop velocity flutter.
- **complexity ← onset rate**: rising-edge counter on the existing `envelopeLevel` (O(1)/block), normalised onsets-per-bar over a 1–2 bar window into a second atomic; input passes a ±0.08 dead-band (the existing `nextDensity` hysteresis, generalised) and is **bar-latched** (applied at step-0 wrap — chatter protection).
- Followed values drive the engine via atomics exposed to the editor (extend the `getCurrentDensity()` pattern in `PluginProcessor.h`); they are **never written back into the parameters** (no host-visible automation feedback).
- Keep the legacy hysteresis Density machine only if the hidden density macro still displays it; otherwise delete dead code.

**RT-safety.** Edge counter and one-pole are O(1) per block; atomics relaxed; bar-latch is the Step 11 mechanism reused.

**Test to add.** EnergyAnalyzer unit: burst-rate signal (click train at 2/s vs 8/s) yields low vs high normalised onset rate; dead-band suppresses ±0.05 wiggle. Processor test: follow on + loud sine ⇒ intensity readout rises, complexity stable; drum output peak amplitude tracks guide level.

**Risk.** Medium — tuning constants (window, dead-band) are musical judgment; encode only directional assertions in tests, not exact values.

**Size.** M.

---

## Step 15 — Sampler failure-path integrity

**Goal.** Fix confirmed **med**: `loadSamples` bad-path clears `anyVoiceLoaded` while the old kit keeps playing, and an empty directory swaps in a silent set (`DrumSampler.cpp:24, 49-56`).

**Files.** `src/drummer/DrumSampler.cpp`; `tests/DrumSamplerTest.cpp`.

**What to build.** On any failure (non-directory, or `!newSet.anyLoaded` after scanning) return `false` **without** swapping and **without** touching `anyVoiceLoaded`. Note the interaction with `autoLoadSamples`'s `areSamplesLoaded()` guard (`PluginProcessor.cpp:200`) — after this fix a failed load can no longer cause a silent kit swap on the next `prepareToPlay`.

**RT-safety.** Loader-thread code only; the swap-under-lock path is unchanged for the success case.

**Test to add.** Load good kit → attempt bad path → assert `areSamplesLoaded()` still true and a processed block is still audible; attempt empty directory → same assertions; both attempts return false.

**Risk.** Low.

**Size.** S.

---

## Step 16 — Sampler resampling at load

**Goal.** Fix confirmed **med**: kits at a different sample rate play pitched/off-tempo (`DrumSampler.cpp:14-17, 115-122` — `currentSampleRate` stored but never used).

**Files.** `src/drummer/DrumSampler.h/.cpp`; `tests/DrumSamplerTest.cpp`.

**What to build.** At load time, **on the loading thread**, resample each file into the new `SampleSet` targeting the prepared rate (`juce::LagrangeInterpolator`, allocate scratch on the loader thread) before the locked swap. Record the rate the set was built for; `prepareToPlay` with a different rate re-triggers `loadSamples` from the remembered path (message/loader thread — with Step 17 this becomes the async job; until then the existing synchronous call sites are acceptable).

**RT-safety.** All resampling off the audio thread; audio path unchanged (still raw 1:1 reads, now correct by construction).

**Test to add.** Write a scratch-dir WAV at 22.05 kHz containing a 1 kHz sine, prepare sampler at 44.1 kHz, load, trigger, FFT-or-zero-crossing assert the rendered pitch is ~1 kHz (not ~500 Hz) and the rendered length ≈ source seconds × 44100.

**Risk.** Low-medium — interpolator edge samples; assert output length within ±16 samples.

**Size.** S.

---

## Step 17 — Async kit loading

**Goal.** Fix confirmed **low**: blocking directory scan + whole-WAV reads inside `prepareToPlay` (`autoLoadSamples`, `PluginProcessor.cpp:78`) and synchronous reload in `setStateInformation` (lines 179–181) stall session load and pluginval's prepare/state cycling.

**Files.** `src/PluginProcessor.cpp/.h`; `src/drummer/DrumSampler.h` (if a load-in-progress query helps); `tests/AdaptiveDrummerProcessorTest.cpp`.

**What to build.** A `triedAutoLoad` flag (reset on explicit user load) so `prepareToPlay` doesn't rescan every prepare; defer kit loading to a `juce::ThreadPool` job (pool member of the processor, 1 thread) guarded by an atomic in-progress flag — the existing C1 swap already makes async completion safe against the audio thread. `setStateInformation` enqueues instead of blocking. **Keep a synchronous path for tests** (`loadSamplesSync` or a test-only flag).

**RT-safety.** Removes I/O from lifecycle callbacks; audio thread untouched. Ensure the pool job never touches APVTS without the message thread (post the `samplesPath` property write back via `MessageManager::callAsync`, or set it before enqueue).

**Test to add.** Processor test: `setStateInformation` with a remembered path returns promptly (< generous wall-clock bound) and the kit is loaded after pumping the message loop / joining the job; repeated `prepareToPlay` with no kit found does not rescan (observable via a counter hook or timing bound). Re-run Step 1 concurrency stress.

**Risk.** Medium — lifecycle/threading; pluginval cycles prepare/release aggressively. Join or cancel the pool job in the destructor.

**Size.** M.

---

## Step 18 — Jazz re-author: swing + velocities

**Goal.** Fix confirmed **low** content finding: Jazz is a straight-16ths rock beat voiced on the ride (`DrumPattern.cpp:73-102`). All infrastructure (swing in the clock — Step 3/11; velocity columns and probability — Step 11) already exists; this step is data authoring.

**Files.** `src/drummer/DrumPattern.cpp` (Jazz table); `tests/DrumPatternTest.cpp` (Jazz-specific invariants updated **in the same commit**).

**What to build.** Jazz table: `swing` ≈ 0.6-triplet feel; ride skip-note pattern with `velLow/velHigh` shaping; snare comping cells at high thresholds with probability < 255 and `velRandRange` ±10; feathered kick (low velHigh). Optionally add the deferred ghost/ornament cells for Rock/Electronic here too.

**RT-safety.** Const data only; zero code.

**Test to add.** Updated Jazz invariants: ride active on beats, snare backbeat **not** required (comping is probabilistic — assert snare fires *somewhere* over 8 bars at complexity 1 with the default seed); swung offsets — odd-step triggers land later than straight positions by the expected sample delta; determinism test still passes.

**Risk.** Low technically; musical quality needs a human ear — flag for review, don't tune by test.

**Size.** S.

---

## Step 19 — `TableExchange` mailbox (dormant) + optional leaves

**Goal.** Future-proofing and delegable extras. None of this blocks release.

**Files.** New `src/drummer/TableExchange.h`; `src/drummer/AdaptiveDrummer.h/.cpp`; optionally `src/PluginProcessor.cpp` + `CMakeLists.txt` (MIDI-out); tests.

**What to build.**
- **TableExchange** (for *future* user-edited grids/fills — dormant until an editable grid ships): preallocated 2-slot lock-free mailbox owning two `GrooveTable` slots. Producer (message thread / ThreadPool job) writes the free slot then `pendingSlot.store(idx, std::memory_order_release)`; consumer (audio thread, at bar wrap only) `exchange(-1, std::memory_order_acquire)` and re-points the active-table pointer. **No SpinLock.** Built-in styles keep using the Step 11 const-pointer path — the mailbox is only for dynamic tables.
- **Optional MIDI-out leaf**: a non-automatable "MIDI Out" bool; when on, map each `(voice, offset, velocity01)` trigger to GM notes (36/38/42/49/51/45) into the `MidiBuffer` that `processBlock` already receives and ignores. Requires `NEEDS_MIDI_OUTPUT TRUE` in `CMakeLists.txt` and `producesMidi()` — **this flips plugin capabilities, so gate the PR on a full pluginval strictness-10 run on both platforms.**

**RT-safety.** Mailbox: audio side is one `exchange` + pointer repoint at bar wrap; producer never blocks the consumer; slots preallocated. MIDI-out: `MidiBuffer::addEvent` into the host-provided buffer is allocation-free for small counts.

**Test to add.** Mailbox unit test: producer thread publishes alternating tables while consumer drains at simulated bar wraps for ~1 s; assert the consumer only ever observes fully-written tables (embed a checksum cell) and no publish is lost when the producer outpaces bars (last-writer-wins). MIDI-out: process a bar, assert the `MidiBuffer` holds the expected notes at the expected sample offsets with velocities matching `vel01 * 127`.

**Risk.** Mailbox: low (dormant). MIDI-out: medium (capability churn under pluginval) — keep it last, keep it optional.

**Size.** M.

---

## Confirmed-finding → step map (verification checklist)

| Finding (severity) | Fixed in |
|---|---|
| B4×B3 ppq re-lock double/dropped triggers (high) | Step 4 (enabled by Step 3; guarded by Step 1) |
| B2 per-voice single cursor / retrigger click (high) | Step 5 |
| D1 no velocity/accent data — 2-D has nothing to modulate (high) | Step 11 (plumbing in Step 5, params in Step 12) |
| No sidechain bus; guide cleared by output (high) | Step 13 |
| A2 per-block `setStyle` rebuild (med) | Step 10 (edge-detect) + Step 11 (const tables + atomic index) |
| Playing-without-ppq treated as stopped (med) | Step 4 |
| `loadSamples` failure corrupts kit state (med) | Step 15 |
| No resampling — pitched/off-tempo kits (med) | Step 16 |
| Synth sum unbounded (~1.9 peak) (med) | Step 6 |
| Step clock untested (med) | Step 1 |
| Phantom 17th step / uneven grid (low) | Step 3 |
| Try-lock scope drops a block's triggers + stale `DrumSampler.h:65` comment (low) | Step 7 |
| 4/4 hardcoded (low) | Step 4 |
| Stop hard-reset click + zero tail (low) | Step 8 |
| Blocking I/O in prepare/setState (low) | Step 17 |
| Synth↔Samples toggle stale tails (low) | Step 8 |
| Follow analyses disabled input (low) | Step 13 |
| Mid-bar structural changes (low) | Step 11 (bar latch) + Step 14 (followed complexity latch) |
| Jazz genre-wrong, no swing (low) | Step 3 (hook) + Step 11 (per-style swing) + Step 18 (content) |
| Follow quantized through 3-step density (low) | Step 14 |
| No state-version marker (low) | Step 12 |
| No C1 concurrency test (low) | Step 1 (re-exercised in Steps 7, 17) |
| No processor-level test (low) | Step 2 (extended in Steps 8, 12, 13, 14, 17) |
