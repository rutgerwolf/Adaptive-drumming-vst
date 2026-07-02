# Fable Review — Confirmed Findings

**Executive summary:** The step clock and voice model are the core risks: ppq re-derivation against integer-truncated pattern lengths double-fires or drops step triggers, and single-slot voices drop same-block hits and click on retrigger — both need the exact-boundary clock rewrite plus a small voice pool, guarded by a new `DrumStepClockTest` landed first.
The product's planned 2-D intensity/complexity axes are blocked by two structural gaps: the pattern model stores only on/off bitmasks (no velocity), and follow-a-track has no sidechain bus so the guide signal is destroyed by the plugin's own output.
Everything else is hardening: redundant per-block pattern rebuilds, host-transport edge cases, kit-load failure paths, missing resampling, synth output exceeding full scale, and a set of low-severity timing, state-migration, and test-coverage gaps that are cheapest to fix now.

---

## High severity

### 1. Host-sync ppq re-derivation vs integer-truncated pattern/step lengths causes double-fired or dropped step triggers

- **Defect:** Every block overwrites `playheadSample` with `llround(frac * truncatedPatternLen)` while the free-running scan advanced exactly `numSamples`, so the two clocks disagree by up to a sample whenever the bar length is non-integral.
- **Location:** `src/drummer/AdaptiveDrummer.cpp:71` (with `src/drummer/DrumPattern.cpp:19`, `src/drummer/DrumStepClock.h:28`)
- **Failure scenario:** At 130 BPM / 44.1 kHz the exact bar is 81415.38 samples but `patternLen` truncates to 81415; consecutive blocks overlap or gap by a sample, so a step boundary (`posInPattern % stepLen == 0`) is scanned twice (flam/retrigger) or never (dropped hit).
- **RT-safety impact:** No allocation or blocking — this is a real-time *correctness* bug: audibly wrong output (flams, missing hits) on the audio thread at common tempos.
- **Fix sketch:** Keep bar/step timing in double precision — exact `patternLen`, boundaries as `llround(k * patternLenExact / 16)` — and fire each step index at most once; alternatively re-lock from ppq only when |ppq-derived − free-run| exceeds a tolerance, free-running between. Add a multi-block host-sync regression test asserting each of 16 steps fires exactly once per bar at non-integral tempos.

### 2. Single cursor/state per voice drops the first of two same-block triggers and hard-resets ringing tails on retrigger (click)

- **Defect:** `triggerVoice` overwrites `playPos`/`triggerOffset` (sampler) and `t`/`phase` (synth) with no polyphony, so only the last trigger per block per voice renders and retriggers reset amplitude discontinuously.
- **Location:** `src/drummer/DrumSampler.cpp:125-129` and `src/drummer/DrumSynth.cpp:24-32`
- **Failure scenario:** At 240 BPM, `stepLen` = 2756, so a 4096-sample buffer holds two hi-hat steps; all triggers run before `mixVoices`, so the first hat never sounds. Retriggering a still-ringing crash (~1.2 s tail) snaps its envelope mid-decay — an audible click.
- **RT-safety impact:** RT-safe as written but musically broken on the audio thread; the fix must stay allocation-free.
- **Fix sketch:** Fixed-size preallocated slot pool per voice (3-4 POD slots, round-robin steal-oldest with a ~2 ms steal fade); no heap on the audio thread. Regression test: two triggers of one voice at different offsets in a single block, assert two onsets.

### 3. Pattern model stores on/off bitmasks only — no velocity/accent data, so a continuous intensity axis has nothing to modulate (D1, crux)

- **Defect:** `uint8_t layers[3][16]` bitmasks with a boolean `getStep` mean intensity can only add/remove hits, collapsing the planned 2-D intensity/complexity axes back into the existing 3-step density.
- **Location:** `src/drummer/DrumPattern.h:49`
- **Failure scenario:** Any attempt to wire a continuous intensity control finds `DrumSynth` hardcoding per-voice gains and `DrumSampler` playing at unity — there is no per-hit velocity to scale, so the feature cannot be built on the current model.
- **RT-safety impact:** None today; the chosen fix must remain a per-trigger table lookup so the audio thread stays regen-free.
- **Fix sketch:** Fixed-size POD master pattern per style: per (voice, step) `{velocity, complexityThreshold}`; a hit sounds iff `complexity >= threshold`, output velocity = `storedVel * intensityCurve`. Thread velocity through `onTrigger(voice, offset, vel)` into both engines; seed thresholds from the 3 existing layers so legacy density points stay mask-equivalent.

### 4. Follow-a-track has no sidechain bus: the guide is the plugin's own input, which processBlock then clears

- **Defect:** The processor declares a single main in/out pair and `buffer.clear()` replaces the analysed guide with drums, so guide audio and drums can never be heard together.
- **Location:** `src/PluginProcessor.cpp:56-66` (bus layout), `src/PluginProcessor.cpp:99-104` (analyse-then-clear)
- **Failure scenario:** A user routes a bass track into the plugin to follow it: the bass is silenced and only drums come out — the done-feature "follow a track" is unusable in any real session.
- **RT-safety impact:** None; this is a bus-architecture gap.
- **Fix sketch:** Add a default-disabled stereo sidechain via `BusesProperties().withInput("Sidechain", stereo, false)`; extend `isBusesLayoutSupported` for it independently; analyse `getBusBuffer(buffer, true, 1)` when enabled, fall back to main input; clear only main-out channels. Re-run pluginval strictness 10 on both platforms (bus layout is its hardest probe).

## Medium severity

### 5. setStyle → loadStyle rebuilds all pattern layers on the audio thread every block (A2)

- **Defect:** `processBlock` unconditionally calls `drummer.setStyle` each callback, memsetting and re-authoring all 3×16 layers every block.
- **Location:** `src/PluginProcessor.cpp:137-138` (via `src/drummer/AdaptiveDrummer.cpp:25-28`, `src/drummer/DrumPattern.cpp:22-31`)
- **Failure scenario:** Wasted work today; the moment roadmap pattern regeneration hooks into this seam, it becomes either an audio-thread RT hazard or a race with an off-thread generator.
- **RT-safety impact:** Alloc-free today so not yet an RT violation, but it is the exact call path where future regen would violate RT constraints.
- **Fix sketch:** Edge-detect: cache last style/density/source as plain audio-thread members and call setters only on change; specify that future dynamic regen builds a `DrumPattern` off-thread and swaps via the DrumSampler C1 try-lock/atomic idiom. Also restructure `DrumPatternTest`'s raw-mask assertions to structural invariants before the D1 velocity change.

### 6. Host reporting isPlaying without a ppq position is treated as stopped (silence, drummer reset every block)

- **Defect:** `hostPlaying` is assigned only inside `if (auto ppq = pos->getPpqPosition())`, though every `PositionInfo` field is optional.
- **Location:** `src/PluginProcessor.cpp:123-127`
- **Failure scenario:** A host that reports playing but omits ppq leaves `hostPlaying = false`; with Play off, the plugin calls `drummer.reset()` every block and outputs silence. Secondary: `setBpm` clamps to [40, 240] (`AdaptiveDrummer.cpp:32`) so host tempi outside that range compute `patternLen` from a different effective tempo than the ppq mapping.
- **RT-safety impact:** None (reset is plain member writes); functional silence bug.
- **Fix sketch:** Hoist `hostPlaying = pos->getIsPlaying()` out of the ppq branch and pass a separate `hasPpq` flag so the drummer free-runs when playing without ppq; clamp (or don't) host bpm consistently before both `patternLen` and the ppq mapping.

### 7. DrumSampler::loadSamples failure paths corrupt kit state: bad path clears anyVoiceLoaded while the old kit keeps playing; empty directory swaps in a silent set

- **Defect:** The not-a-directory early return stores `anyVoiceLoaded = false` without touching `sampleSet`, while a WAV-less directory still swaps in an all-silent `SampleSet`.
- **Location:** `src/drummer/DrumSampler.cpp:24` (flag cleared on early return) and `src/drummer/DrumSampler.cpp:49-56` (unconditional swap)
- **Failure scenario:** User mistypes a kit path: UI reports "no kit" while audio keeps playing the old kit, and `autoLoadSamples`' guard (`PluginProcessor.cpp:200`) may then silently swap kits on the next `prepareToPlay`. Pointing at an empty folder unloads a working kit into silence.
- **RT-safety impact:** None directly; state-consistency bug between message thread and audio behaviour.
- **Fix sketch:** On any failure (non-directory or `!newSet.anyLoaded`) return false without swapping and without touching `anyVoiceLoaded`; unit test: load good kit, attempt bad path, assert `areSamplesLoaded()` still true and output still audible.

### 8. DrumSampler never resamples: kits at a different sample rate than the host play pitched and off-tempo

- **Defect:** `prepare()` stores `currentSampleRate` but no code reads it; frames are read raw and copied 1:1.
- **Location:** `src/drummer/DrumSampler.cpp:14-17` (stored, never used) and `src/drummer/DrumSampler.cpp:115-122` (raw frame read)
- **Failure scenario:** A 44.1 kHz kit in a 48 kHz session plays ~8.8% slow and flat while the step clock (host SR) stays on time — every hit is detuned and its tail drags against the grid.
- **RT-safety impact:** None at render time; the fix must resample off the audio thread.
- **Fix sketch:** Resample at load time on the loading thread (`juce::LagrangeInterpolator`) into the new `SampleSet` targeting the prepared rate, before the SpinLock swap; re-trigger `loadSamples` from `prepareToPlay` when the rate changed.

### 9. DrumSynth voice sum is unbounded — dense steps exceed full scale (~1.9 peak after default volume)

- **Defect:** The synth mix bus sums per-voice signals with hardcoded gains and no limiter, exceeding ±1.0 on dense steps.
- **Location:** `src/drummer/DrumSynth.cpp:51-65`
- **Failure scenario:** Rock/Electronic Full step 0 (Crash|Kick|HiHat, `DrumPattern.cpp:63/126`) peaks 1.69-2.35 pre-volume (~1.9 after the 0.8 default gain) — output beyond full scale into the host on every downbeat at full density; the differentiated noise (range ±2) makes the hat alone reach ~1.0.
- **RT-safety impact:** None; digital clipping / hot output into the host chain.
- **Fix sketch:** Rebalance per-voice gains for ~-6 dB headroom and/or add a cheap per-sample soft clip (`tanh` or `s/(1+|s|)`) on the mix bus; add a test asserting max |sample| ≤ 1.0 for the densest step across styles.

### 10. Shared step clock (forEachStepTrigger) has zero direct tests — the timing-critical unit is unguarded ahead of the required host-sync rewrite

- **Defect:** `tests/` contains only pattern/drummer/sampler/synth/analyzer tests that exercise the clock indirectly, yet the confirmed high-severity timing fixes must rewrite exactly this code.
- **Location:** `src/drummer/DrumStepClock.h:31` (no `tests/DrumStepClockTest.cpp`)
- **Failure scenario:** The B4 clock rewrite introduces a block-edge double-fire or a wrap-mid-block miss; CI stays green and the regression ships.
- **RT-safety impact:** Indirect — unguarded changes to the hottest audio-thread loop.
- **Fix sketch:** Land `DrumStepClockTest.cpp` *before* touching the clock: counting lambda, sweep block sizes (1/64/480/4096) and non-integral-bar tempos, assert 16 firings per bar at exact offsets and exactly-once across block boundaries.

## Low severity

### 11. Phantom 17th step boundary and uneven grid when patternLen % 16 != 0

- **Defect:** Integer `stepLen` leaves a remainder region whose start fires `getStep(16)`, silent only via the pattern's bounds check, and compresses the grid by up to ~15 samples.
- **Location:** `src/drummer/DrumStepClock.h:28-37` (bounds check at `src/drummer/DrumPattern.cpp:12`)
- **Failure scenario:** At 130 BPM / 44.1 kHz, `patternLen` = 81415, `stepLen` = 5088; position 81408 fires step index 16 — any future code trusting `posInPattern / stepLen` inherits an out-of-range index.
- **RT-safety impact:** None today (inaudible, bounds-checked); latent out-of-range index hazard.
- **Fix sketch:** Compute boundaries as `llround(k * exactPatternLen / 16)` walking a `nextStepSample` forward (also removes two per-sample modulos); folds into the high-severity host-sync fix; cover with the new clock test at a non-divisible tempo.

### 12. Sampler try-lock scopes over the whole render: a missed lock permanently drops that block's triggers, and the DrumSampler.h:65 comment misdocuments the critical section

- **Defect:** `ScopedTryLockType` wraps `forEachStepTrigger + mixVoices` while `AdaptiveDrummer` advances `playheadSample` regardless, so a swap-coincident block loses its hits forever, and the header claims the lock is "held only for the cheap swap".
- **Location:** `src/drummer/DrumSampler.cpp:67-73` (stale comment at `src/drummer/DrumSampler.h:65`)
- **Failure scenario:** A kit load lands mid-block: the audio thread's try-lock fails, that block's step triggers are skipped (not just its audio), and the beat has a one-off hole; meanwhile the message thread's blocking SpinLock can spin for a full block render.
- **RT-safety impact:** RT-safe as designed (try-lock never blocks audio) but causes rare musical dropouts; the misleading comment invites a future blocking-lock "fix".
- **Fix sketch:** Run `forEachStepTrigger` outside the lock (it reads only pattern + playhead) into a small fixed array, take the try-lock only around `mixVoices` — or hold `SampleSet` behind an atomically swapped ref; at minimum fix the header comment.

### 13. Bar mapping hardcodes 4/4: host time signature and bar start never queried

- **Defect:** `ppqToPlayhead` always divides by 4 beats and the processor reads only bpm/ppq/isPlaying.
- **Location:** `src/drummer/AdaptiveDrummer.cpp:51-63` (`beatsPerBar` default 4.0); `src/PluginProcessor.cpp:114-128` (never reads `getTimeSignature`)
- **Failure scenario:** In a 3/4 or 6/8 session the pattern's downbeat drifts one beat per host bar; low because the patterns are authored 4/4 anyway.
- **RT-safety impact:** None.
- **Fix sketch:** Read `pos->getTimeSignature()` (and `getPpqPositionOfLastBarStart()` when present), pass `beatsPerBar` through `setHostTimeline` into `ppqToPlayhead` (the signature already takes it and is unit-tested); add 3/4 test cases.

### 14. Transport stop hard-resets voices every block: tails truncated with a click, getTailLengthSeconds()==0, redundant per-block reset

- **Defect:** The stopped branch calls `drummer.reset()` every block and the processor reports zero tail length.
- **Location:** `src/PluginProcessor.cpp:155` (and `src/PluginProcessor.h:44`)
- **Failure scenario:** User stops the transport while a crash rings: the tail is cut at a block boundary (step discontinuity click), and because `getTailLengthSeconds()` returns 0.0 the host renders no tail either.
- **RT-safety impact:** None (`reset` is alloc-free); audible artifact on stop.
- **Fix sketch:** Edge-detect playing true→false: rewind the playhead once, suppress `forEachStepTrigger` but keep rendering active voices until they self-terminate; report a realistic tail (~1.2 s crash decay).

### 15. Blocking sample-file I/O in prepareToPlay and setStateInformation; directory rescan on every prepare when no kit is found

- **Defect:** `autoLoadSamples` does directory iteration plus whole-WAV reads inside `prepareToPlay`, and `setStateInformation` synchronously reloads the remembered kit.
- **Location:** `src/PluginProcessor.cpp:78` (`autoLoadSamples` in `prepareToPlay`) and `src/PluginProcessor.cpp:179-181` (synchronous reload in `setStateInformation`)
- **Failure scenario:** Session load or pluginval's prepare/state cycling stalls on disk I/O; the `areSamplesLoaded` early-out helps only after a successful load, so a missing kit rescans every prepare.
- **RT-safety impact:** Never touches the audio callback (the C1 try-lock covers handover) — a message/prepare-thread stall, not an RT violation.
- **Fix sketch:** Add a `triedAutoLoad` flag (reset on explicit user load) and defer kit loading to a background thread/ThreadPool job guarded by an atomic in-progress flag; the existing C1 swap already makes async completion safe. Keep a synchronous path for tests.

### 16. Toggling Synth↔Samples freezes the inactive engine's voices; stale tails resume audibly on toggle-back

- **Defect:** `setUseSynth` only flips the flag and `processBlock` stops calling the other engine, freezing a sampler `playPos` or synth `{active, t}` mid-decay.
- **Location:** `src/drummer/AdaptiveDrummer.h:33` (`setUseSynth`) and `src/drummer/AdaptiveDrummer.cpp:74-79`
- **Failure scenario:** User toggles to samples during a synth crash tail, then toggles back seconds later: the crash fragment resumes from mid-decay — a hit from nowhere.
- **RT-safety impact:** None; both engine resets are plain member writes.
- **Fix sketch:** Edge-detect the flag change (in `setUseSynth` or `processBlock`) and `reset()` the engine being switched away from; add a toggle round-trip test asserting silence until the next step boundary.

### 17. Follow analyses the main buffer without checking the input bus is enabled

- **Defect:** The layout explicitly permits a disabled main input (`PluginProcessor.cpp:65`) yet `energyAnalyzer.processBlock` unconditionally reads the main buffer, whose content is then host-defined.
- **Location:** `src/PluginProcessor.cpp:99-100`
- **Failure scenario:** With the input bus disabled, the buffer may contain stale previous output in some hosts — the analyzer follows the plugin's own drums (self-follow feedback loop).
- **RT-safety impact:** None; undefined-input correctness issue.
- **Fix sketch:** Gate analysis on the input bus being enabled with >0 channels, else `processSilence`; add a processor-level test with input disabled asserting energy stays ~0 with follow on.

### 18. Structural pattern changes (density/style) apply mid-bar at arbitrary block boundaries

- **Defect:** `setDensity`/`setStyle` take effect on the very next step rather than at a bar boundary.
- **Location:** `src/PluginProcessor.cpp:144-148`
- **Failure scenario:** Follow (or a future continuous puck) flips hat subdivision or crash layers mid-bar — musically wrong, mitigated today only by the analyzer's hysteresis.
- **RT-safety impact:** None; musical design issue.
- **Fix sketch:** Latch structural changes into a pending value applied when the step clock wraps to step 0 (`AdaptiveDrummer` already knows `posInPattern`); continuous intensity (velocity scaling, post-D1) can stay immediate.

### 19. Jazz patterns are straight 16ths with backbeats — no swing infrastructure, genre-wrong content

- **Defect:** Jazz Medium/Full are straight 8th/16th ride grids with snare on steps 4/12 — a rock beat voiced on the ride — and no swing offset exists anywhere in the clock.
- **Location:** `src/drummer/DrumPattern.cpp:73-102` (no swing support in `src/drummer/DrumStepClock.h`)
- **Failure scenario:** Selecting Jazz produces a straight rock feel on ride cymbal; no code path can produce swung placement.
- **RT-safety impact:** None; content/product-quality finding.
- **Fix sketch:** Add per-style swing consumed at boundary computation (delay odd 16ths by `swing * stepLen` — a natural fit with the exact-boundary clock rewrite); re-author Jazz with swung skip notes and comped snare once D1 velocity lands, updating the Jazz mask tests in the same commit.

### 20. Follow control path quantizes the continuous energy through the 3-step density state — the 2D engine cannot be steered through it

- **Defect:** Energy is already a continuous 0..1 atomic but the follow wiring consumes only the hysteresis-quantized `Density`, re-collapsing both planned axes.
- **Location:** `src/PluginProcessor.cpp:144-148` (`density = energyAnalyzer.getDensity()`); `src/drummer/EnergyAnalyzer.cpp:64-84`
- **Failure scenario:** Even after D1 lands, Follow can only step between three densities — the continuous intensity axis remains unreachable from the analyzer.
- **RT-safety impact:** None; roadmap design gap.
- **Fix sketch:** Expose the smoothed energy as Follow-driven intensity directly, keep the hysteresis machine for legacy density, and add a cheap onset-rate feature (envelope-rise count per bar) as the complexity input — all existing atomics, O(1) per block.

### 21. No state-version marker in saved state ahead of the planned density → intensity/complexity migration

- **Defect:** `getStateInformation` serialises the raw APVTS tree with no schema/version property.
- **Location:** `src/PluginProcessor.cpp:164-169`
- **Failure scenario:** After the D1/intensity migration ships, sessions saved today cannot be distinguished from new-format state, forcing guesswork migration; cheap to add now before any state exists in the wild.
- **RT-safety impact:** None.
- **Fix sketch:** `setProperty("stateVersion", 1)` before serialising, read it (absent = v1) in `setStateInformation` and branch migration; add intensity/complexity as NEW ParameterIDs and keep "density" mapped for one release for VST3 ID stability.

### 22. No concurrency regression test for the C1 locked-swap that the roadmap designates as the template for dynamic pattern regen

- **Defect:** `DrumSamplerTest` covers B1 and the missing-kit case single-threaded only; nothing guards the alloc-outside-lock / free-on-loader-thread invariants.
- **Location:** `tests/DrumSamplerTest.cpp:1`
- **Failure scenario:** A refactor moves allocation or destruction inside the lock (or onto the audio thread); CI stays green and the RT-safety invariant silently breaks.
- **RT-safety impact:** Indirect — the untested invariants are exactly the RT-safety guarantees of the swap.
- **Fix sketch:** Test running `processBlock` in a loop on one thread while `loadSamples` alternates two temp kits (long/short WAVs) on another for ~1 s; assert no crash, finite output, post-swap blocks use the new kit; optionally add a TSan CI job.

### 23. No processor-level test (bus layout / follow routing / state round-trip) below pluginval

- **Defect:** `tests/` has no `PluginProcessor` coverage, and the sidechain step must rewrite `isBusesLayoutSupported` — the most common way to go red on pluginval strictness 10 — with no CTest net to localise failures.
- **Location:** `src/PluginProcessor.cpp:56` (no processor test exists in `tests/`)
- **Failure scenario:** The sidechain-bus change breaks a layout combination; the first signal is a pluginval failure with no unit-level localisation.
- **RT-safety impact:** Indirect — missing safety net for host-facing changes.
- **Fix sketch:** Add `AdaptiveDrummerProcessorTest`: assert accepted/rejected layouts (mono/stereo/disabled-in, later +sidechain), prepare+process with a sine guide and follow on asserting density rises, and round-trip get/setStateInformation asserting params + samplesPath survive.
