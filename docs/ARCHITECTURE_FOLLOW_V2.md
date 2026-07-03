# ARCHITECTURE_FOLLOW_V2 — Grid-preserving Follow: guide-driven accents + intensity contour

> **Status.** Replacement design for `ARCHITECTURE_2D_FOLLOW.md` §3.4 ("Analysis → axis mapping") and `ROADMAP_2D.md` Step 14, neither of whose Follow mapping ever shipped. The user requirement that forces the change, verbatim:
>
> *"Follow status must not override the grid. The grid still determines the base groove; the followed track determines where accents land and when it becomes more or less intense. Research how GarageBand does this."*
>
> The old plan (energy→intensity directly, onset-rate→complexity directly) made Follow a full *replacement* of both axes. This design makes Follow a bounded *modulation layer* on top of the grid: **complexity is never followed**, the base groove is always the XY pad's, and the guide contributes two new signals — a per-hit **accent** channel and a phrase-level **intensity contour** — both velocity-only, both scaled by a new **Follow Amount** parameter.
>
> Everything in §3.1–§3.3 of the original doc (sidechain bus topology, guide priority chain, per-DAW routing) shipped in Step 13 and is unchanged. Only the mapping seam is redesigned.

---

## 1. Reference behavior — how GarageBand / Logic Drummer actually does it

Verified against Apple's current documentation (not assumed):

- The **X/Y pad (Complexity × Loudness) always remains the primary control** for pattern character — busyness, ghost notes, overall level. Follow never takes it over.
- **"Follow Rhythm" is scoped**: only "the kick and snare portions of a Drummer region follow another track." Hi-hat, ride, ghosts, fills — everything else stays exactly as the pad set it. Follow influences a *subset of voices*, layered on the authored groove.
- There is a **follow-strength slider**: "at the left, the Drummer loosely references the track"; further right it locks tighter. Follow is a *blend*, not a switch.
- Apple recommends monophonic/percussive guides (bass, muted guitar) — i.e. the feature is designed around a guide whose transients are musically meaningful events.

Sources: [Apple Support — Set Drummer to follow another track's rhythm in Logic Pro](https://support.apple.com/guide/logicpro/follow-the-rhythm-of-another-track-lgcp45310225/mac), [Logic Pro Help — Follow the rhythm of another track](https://logicpro.skydocu.com/en/add-drummers-to-your-project/work-in-the-drummer-editor/follow-the-rhythm-of-another-track/).

**Design principles extracted** (adapted to our signal — a continuous guide-energy level, not a MIDI transcription):

1. The grid is authoritative; Follow is a scoped, bounded overlay.
2. Follow targets the voices that carry emphasis (their kick & snare ≈ our `kFlagAccent` cells), not the whole kit equally.
3. Follow has a user-facing strength control.
4. Since we cannot *re-time* hits without changing which cells fire (forbidden), our adaptation of "kick & snare follow the rhythm" is: **the guide's transients decide which of the already-firing hits get emphasized**, and the guide's phrase-level loudness decides how hard the whole (unchanged) pattern plays.

---

## 2. Overview

```
                                 message thread
        APVTS: complexity ── ALWAYS the structural axis (Follow never writes it)
               intensity ─── the BASE dynamics axis
               followAmount ─ 0..1 strength of both modulations
                                 audio thread (processBlock, existing order)
 guide bus ──▶ EnergyAnalyzer ──▶ fast energy   (existing 30/300 ms env — meter + accent input)
                              ├─▶ macro energy  (NEW 250/800 ms env — phrase-level contour)
                              └─▶ accent        (NEW = clamp01((fast − macro − margin)·gain))
                                       │
      intensityEff = base + blend·(macro − base)          ── "more/less intense"
      per-trigger:  vel += depth·weight(cell)·(accent − neutral)  ── "where accents land"
                                       │
      stepFires(complexity_manual)  ◀── UNCHANGED: which cells fire is the grid's alone
```

Two independent modulation channels, deliberately separated by time scale:

- **Intensity contour** (slow, ~0.25–0.8 s): the guide's phrase-level loudness pulls the *effective intensity* toward itself, bounded around the grid's base value. Non-structural; composes with the existing re-voicing lerp so ghosts stay ghosts by authoring.
- **Accent channel** (fast, ~0.1–0.3 s): the guide's transients — moments where it pokes above its own running level — add a bipolar velocity term to eligible cells at the step boundaries that coincide with them. `kFlagAccent` cells get full weight; ghosts get zero.

The two-envelope structure makes the hand-off automatic: a sudden loud section reads as *accent* for the first ~300 ms (fast env leads), then transfers to *intensity* as the macro envelope catches up. That is exactly a drummer's response — hit the moment, then settle into playing louder.

---

## 3. Guide analysis — `EnergyAnalyzer` additions

### 3.1 Why not the old rising-edge-counter sketch

Step 14's sketch (rising-edge counter → onsets-per-bar over a 1–2 bar window) answered a different question: *how busy is the guide on average*, to drive complexity. Complexity is now off the table, and for accent placement a rate is useless — we need *"is a transient happening right now, and how strong."* We also do **not** need free-running onset event detection with nearest-step quantization: the drum grid itself samples time at every 16th boundary, so a **continuous transient-salience signal, sampled at step boundaries**, gives beat-quantized accents with no event list, no timestamps, no windows, no counters — strictly less machinery than either the old sketch or a discrete onset detector, and it carries a magnitude, which a binary edge counter cannot.

The cheapest correct salience signal from the existing pipeline is the classic **two-envelope difference**: a fast envelope (we already have it — `envelopeLevel`, 30 ms attack / 300 ms release) minus a slow "context" envelope of the same normalized-dB target. When a note attacks, the fast envelope jumps while the slow one lags → positive difference for ~100–300 ms → any step boundary inside that window sees the accent. It is self-normalizing (accent = louder *than the guide's own recent level*, so a hot-mixed guide doesn't read as constant accents), and the slow envelope is *the same signal* the intensity channel needs — one new one-pole serves both channels.

### 3.2 Exact computation (per block, O(1), no allocation)

`updateEnvelope()` grows from one envelope to two plus a derived signal. `target` is the existing normalized-dB value; both `processBlock` and `processSilence` route through here unchanged.

```cpp
// EnergyAnalyzer.h — new tuning constants (all by-ear tunables; see §11)
static constexpr float kMacroAttackSec  = 0.250f;   // phrase-level rise
static constexpr float kMacroReleaseSec = 0.800f;   // phrase-level fall
static constexpr float kAccentMargin    = 0.05f;    // rejects envelope jitter on steady input
static constexpr float kAccentGain      = 2.5f;     // (fast − macro) ≈ 0.4 on a hard accent → ≈ 1.0

// new members
float              macroLevel { 0.0f };             // audio thread only
std::atomic<float> macroEnergy { 0.0f };            // lock-free, editor-readable
std::atomic<float> accent      { 0.0f };            // lock-free, editor-readable

// EnergyAnalyzer.cpp — updateEnvelope(targetEnergy, numSamples), after the existing fast update:
const float macroTc    = (targetEnergy > macroLevel) ? kMacroAttackSec : kMacroReleaseSec;
const float macroCoeff = std::exp (float (-blockSec) / juce::jmax (1.0e-4f, macroTc));
macroLevel = targetEnergy + macroCoeff * (macroLevel - targetEnergy);

const float m = juce::jlimit (0.0f, 1.0f, macroLevel);
macroEnergy.store (m, std::memory_order_relaxed);
accent.store (juce::jlimit (0.0f, 1.0f, (e - m - kAccentMargin) * kAccentGain),
              std::memory_order_relaxed);           // e = the fast energy already computed
```

New accessors, same idiom as `getEnergy()`:

```cpp
float getMacroEnergy() const noexcept { return macroEnergy.load (std::memory_order_relaxed); }
float getAccent()      const noexcept { return accent.load      (std::memory_order_relaxed); }
```

Cost: one extra `std::exp`, two extra relaxed stores per block. Behavior checks (numbers from the actual time constants):

- **Steady guide at any level**: fast and macro converge → difference → 0 < margin → accent settles to 0. A steady pad/drone produces *no* accents, only an intensity level. Correct.
- **Note attack after a quieter passage** (target 0.3 → 0.8): fast reaches ~0.78 in ~100 ms while macro is at ~0.47 → accent ≈ `(0.31 − 0.05)·2.5` ≈ **0.65** at the next boundary; decays over ~300 ms as macro catches up. One 16th at 120 BPM is 125 ms, so the accent covers the coincident boundary and has largely faded by the one after — accents land on hits, not on regions. (Above ~180 BPM some bleed onto the following 16th is possible; accepted for v1, see §11.)
- **Sustained loud section**: accent fires once at the entry, then hands off to `macroEnergy` — the intensity channel holds the elevation for as long as the section lasts (no adaptive-baseline decay; a 16-bar loud chorus stays intense for 16 bars).
- **Silence / `processSilence`**: both envelopes decay to 0, accent → 0.

### 3.3 Deletion: the 3-step density machine

The redesigned seam (§7) removes the last runtime consumer of `getDensity()` (`PluginProcessor.cpp:215`). Per Step 14's own instruction ("otherwise delete dead code"): delete `densityState`, `getDensity()`, `nextDensity()`, and the four hysteresis threshold constants from `EnergyAnalyzer`, and delete/replace the hysteresis tests in `EnergyAnalyzerTest` **in the same commit**. `DrumPattern::Density` and `mapLegacy` stay — they are still needed by the hidden `density` parameter macro and the v1-state migration in `setStateInformation`, neither of which touches the analyzer.

---

## 4. Accent placement — how a guide transient becomes an accented hit

### 4.1 Mechanism: sample the accent signal at each trigger, add a bipolar, cell-weighted velocity term

Chosen shape: no discrete onset events, no nearest-step search. The analyzer's `accent` value is pushed into `DrumPattern` once per block (like intensity); `stepVelocity()` — which is already evaluated exactly at each step boundary by `forEachStepTrigger` — adds a term for that hit. Timing correctness is structural: within `processBlock`, the analyzer runs **before** the drummer renders, so an onset earlier in the same block, or in any previous block, has already raised `accent` when the block's boundaries are evaluated. An onset that lands a few ms *after* a boundary credits the *next* boundary, one 16th later — the causal best possible without lookahead latency (PDC stays 0), and musically benign because the next grid position is where a drummer would place the response anyway.

### 4.2 Eligibility and weights

```
weight(cell) = 0.0   if (flags & kFlagGhost)     — ghosts are texture; accenting one is an oxymoron
             = 1.0   if (flags & kFlagAccent)    — the authored emphasis skeleton (kick 1, backbeats, crash)
             = 0.4   otherwise                   — kAccentWeightRegular: backbone/hat cells get a lean, not a hit
```

Rationale: strict `kFlagAccent`-only would make Follow mute whenever the guide accents a position where no flagged cell fires (e.g. an off-beat where only a hat tick plays) — but a real drummer leans on *whatever they're playing* at that moment. The 0.4 middle tier gives that, while the flags keep the strong voices dominant, mirroring Logic's kick-&-snare scoping. This slots into the free real estate the brief identified: **this is the first runtime consumer of `StepCell::flags`** — update the header comment at `DrumPattern.h:41` ("informational; reserved for later steps") accordingly.

**Authoring gap that must ship in the same commit** (data-only, does not affect `stepFires` or mask-equivalence): Jazz has *zero* `kFlagAccent` cells and Electronic's backbeats lack the flag. Add `kFlagAccent` to Jazz kick[0], snare[4], snare[12] and Electronic snare[4], snare[12]. Without this, the accent channel is silent on Jazz.

### 4.3 Exact formula and why it is bipolar

```cpp
// DrumPattern.h
static constexpr float kAccentNeutral       = 0.25f;  // accent value at which the term vanishes (public, for tests)
static constexpr float kAccentWeightRegular = 0.4f;

/** Guide-accent modulation. accent01 = analyzer accent signal; depth01 = 0 disables (default).
    Velocity-only — stepFires() is untouched by construction. */
void setFollowAccent (float accent01, float depth01) noexcept;   // plain members, audio thread

// DrumPattern.cpp — stepVelocity(), after the lerp + humanization, before the final clamp:
if (followAccentDepth > 0.0f)
{
    const float w = (cell.flags & StepCell::kFlagGhost)  != 0 ? 0.0f
                  : (cell.flags & StepCell::kFlagAccent) != 0 ? 1.0f
                  : kAccentWeightRegular;
    vel += followAccentDepth * w * (followAccent - kAccentNeutral);
}
return juce::jlimit (0.0f, 1.0f, vel);
```

**Why bipolar (± around a neutral point) instead of a pure additive boost.** The cells we most want to accent are nearly saturated by authoring: accent-flagged cells have `velHigh` 124–127, and at the default grid position (intensity 0.55 → `intensityShaped` ≈ 0.66) the rock kick already sits at velocity ≈ 0.92. A purely additive boost has ≤ 0.08 of headroom ≈ **+1 dB** through the engines' `vel^1.5` loudness map — inaudible as an accent. The bipolar term creates contrast from both sides: *between* guide accents (accent ≈ 0) eligible cells sit `depth·w·0.25` *below* their manual level; *on* a guide accent (accent → 1) they get `depth·w·0.75` upward (clamped at 1.0). At the default depth this yields a ~2.5–3 dB swing on flagged cells — audible — and it is exactly how drummers produce accents at high dynamics: lighten the unaccented notes. It degrades coherently too: a guide with no transients (or silence) plays the kit slightly tamer and flatter, i.e. "the band is holding back, so is the drummer."

Notes:
- No new hash draw — the accent term is deterministic given the guide; `stream 2` in `GrooveHash` stays free.
- The term rides on top of humanization (`velRandRange` ±0.06–0.12); accents still read because they correlate with the guide while humanization is uncorrelated.
- `setFollowAccent (kAccentNeutral, anyDepth)` and `setFollowAccent (x, 0)` are both exact no-ops — the defaults (0, 0) make every existing `DrumPatternTest` velocity assertion pass unmodified, which is the migration guard.
- Implementation note (confirmed once actually built): near-saturated authored cells (e.g. Rock kick[0], velLow/velHigh 98/127) can have the *upward* half of this term clamped away by the final `[0,1]` clamp at moderate-to-high intensity — exactly the near-saturation case this bipolar design exists for. The downward half is unaffected. This is expected, not a bug; tests that compare cross-cell swing magnitude should do so on the downward side to avoid clamp interactions.

---

## 5. Intensity contour — "when it becomes more or less intense"

The base always comes from the grid. The guide's **macro** energy pulls the effective intensity toward itself, with bounded authority:

```cpp
// PluginProcessor.cpp, per block (audio thread)
const float blend = kMaxIntensityBlend * followAmount;                    // kMaxIntensityBlend = 0.75
intensityEff      = baseIntensity + blend * (macroEnergy - baseIntensity); // == lerp(base, macro, blend)
```

**Why a lerp toward the guide, not an additive delta around an anchor or a multiplicative envelope:**

- It is the Logic blend-slider semantic exactly ("loosely references ↔ locks tightly"), with `followAmount` as the dial.
- It needs no "neutral energy" anchor constant (the base itself is the anchor: the formula is `base + blend·(macro − base)`), and it is bounded in [0,1] by construction — no clamp interactions.
- A silent guide pulls intensity *down* toward `(1 − blend)·base` and a loud one *up* toward `base + blend·(1 − base)`: the grid's Y position remains the center of gravity at every `followAmount` below 1. The 0.75 ceiling means even at maximum Follow the grid keeps ≥ 25% authority — the literal reading of "the grid still determines the base groove." (Genuine judgment call: 1.0 at max would be Logic's "locks tightly"; see §11.)

**Why it composes safely with the re-voicing formula (the ghost question):** `intensityEff` is fed into the *existing* `setIntensity()` → `lerp(velLow, velHigh, intensity^0.7)` path. Re-voicing means every cell moves only within its authored span: a ghost authored (10, 26) cannot exceed 26/127 no matter how loud the guide gets — **ghosts stay ghosts at guide fortissimo by authoring, not by a runtime guard.** This is precisely why the modulation must enter as the intensity *input* and not as a post-lerp velocity/gain multiplier: a post-multiplier would break authored spans, scale the humanization noise, and (in the sampler, where `gain = velocity`) risk >1.0 slot gains. Accent hits *are* allowed to exceed their authored span up to 1.0 — but only via the explicit, ghost-excluded accent term of §4, which is the "unless that's specifically desired for accent hits" carve-out.

**Smoothing:** no new `SmoothedValue`. The followed component is already smoothed by the macro envelope (250/800 ms — deliberately slower than the fast meter envelope so per-note guide dynamics don't pump the kit; that job belongs to the accent channel). The manual component behaves exactly as today (per-block param read; velocity is per-trigger, so there is nothing to zipper). `volumeSmoothed` is untouched. One consequence to note in a comment: under Follow, `intensityEff` changes nearly every block, so `setIntensity`'s `exactlyEqual` early-out stops saving the `powf` — one `powf` per block is fine.

---

## 6. Complexity: explicitly untouched

**`complexity01` is always the manual APVTS/grid value. Follow contributes nothing to it, ever.** Consequences across the existing code:

- `stepFires()` and `probScale(complexity)`: completely unchanged. The set of firing cells — including the hash-gated ghost/ornament draws, whose probability scales with complexity — is a pure function of (grid complexity, style, seed, barIndex) with no guide input on any path. This is enforced by construction: no new code touches `complexity255` or the probability draw.
- `drummer.setComplexity()` is called with the raw parameter value on every block in both Follow states.
- **The planned bar-latch for followed complexity is deleted from the plan** (architecture doc §2.2 note, §3.4, threading table row "complexity (followed)", roadmap §0 "Structural changes (style, table pointer, followed complexity)"). It existed solely because followed complexity was structural and chatter-prone. Both surviving Follow channels are velocity-only (non-structural), so they follow the same "apply immediately" rule as manual intensity. **No new bar-latch machinery, no dead-band, no pending atomics.** Style keeps its existing latch; nothing else needs one.
- The ±0.08 dead-band and onsets-per-bar window from the old plan die with it.
- The analyzer's 3-step density machine loses its last consumer and is deleted (§3.3).

---

## 7. The new `processBlock` seam (replaces the old `if (follow) mapLegacy(...) else {...}` block)

```cpp
// Guide priority chain — existing code, plus one new fact it already knows:
bool guideAvailable = false;
if (follow)
{
    if (sidechainConnected)      { energyAnalyzer.processBlock (getBusBuffer (buffer, true, 1), numSamples); guideAvailable = true; }
    else if (mainInputEnabled)   { energyAnalyzer.processBlock (mainOut, numSamples);                        guideAvailable = true; }
    else                           energyAnalyzer.processSilence (numSamples);
}
else                               energyAnalyzer.processSilence (numSamples);

// ── The Follow seam ───────────────────────────────────────────────────────────
const float baseComplexity = *apvts.getRawParameterValue ("complexity");
const float baseIntensity  = *apvts.getRawParameterValue ("intensity");
const float followAmount   = *apvts.getRawParameterValue ("followAmount");   // Step 21; constant 0.6 in Step 20

float intensityEff = baseIntensity;
float accent01     = 0.0f;
float accentDepth  = 0.0f;

if (follow && guideAvailable)   // no guide bus at all ⇒ Follow modulates nothing (manual passthrough)
{
    const float blend = kMaxIntensityBlend * followAmount;        // 0.75 * amount
    intensityEff = baseIntensity + blend * (energyAnalyzer.getMacroEnergy() - baseIntensity);
    accent01     = energyAnalyzer.getAccent();
    accentDepth  = kMaxAccentDepth * followAmount;                // 0.40 * amount
}

drummer.setComplexity   (baseComplexity);          // ALWAYS the grid — the hard constraint, in one line
drummer.setIntensity    (intensityEff);
drummer.setFollowAccent (accent01, accentDepth);   // new passthrough → DrumPattern

currentComplexityState.store (baseComplexity, std::memory_order_relaxed);
currentIntensityState.store  (intensityEff,   std::memory_order_relaxed);
```

`AdaptiveDrummer` gains a one-line passthrough (`setFollowAccent(a, d) { drumPattern.setFollowAccent(a, d); }`, immediate like `setIntensity` — see §6 for why no latch). The processor exposes `float getGuideAccent() const { return energyAnalyzer.getAccent(); }` for the editor, alongside the existing `getEnergy()`. The `guideAvailable == false` behavior (Follow on, but no sidechain and main input disabled) is a deliberate improvement on today: instead of following silence down to Sparse, Follow becomes a passthrough of the manual values — "no guide, nothing to follow," matching the architecture doc's graceful-degradation intent. A *connected but silent* guide, by contrast, does modulate (downward) — that is the "band stopped playing" response, and it is intentional.

**`processSilence` when Follow is off** keeps today's semantics (meter decays; now also macro/accent decay), so toggling Follow on never starts from stale analysis.

---

## 8. New parameter: `followAmount` — recommendation: yes, one parameter

**Add it** (`ParameterID { "followAmount", 3 }`, Float 0–1, step 0.01, default **0.6**, name "Follow Amount"). Version hint 3 per the established convention (hint = release that introduced the param; intensity/complexity used 2). No state migration needed: sessions saved before it simply get the default (unlike `density`, there is no legacy carrier of its meaning).

Reasoning (not punting):
- The two modulation depths need scale constants regardless; a single parameter multiplying both (`accentDepth = 0.40·amount`, `blend = 0.75·amount`) costs one knob and removes the biggest tuning risk — guide material varies enormously (clean bass vs. dense mix), and no fixed depth is right for both. This is also exactly the control Logic ships, which the user pointed us at as the reference.
- It fixes the on/off cliff: a bare toggle at a fixed depth makes enabling Follow jarring; a depth makes it mixable and automatable ("push Follow in the chorus").
- **One** parameter, not separate accent/intensity depths: two knobs would demand users understand the internal split; the fixed 0.40/0.75 ratio between the channels is an authored voicing decision, revisited by ear, not exposed.

---

## 9. Threading & RT-safety

| Value | Written by | Read by | Applied |
|---|---|---|---|
| fast energy (existing) | analyzer, audio thread | audio seam + editor meter (relaxed atomic) | per block |
| **macroEnergy (new)** | analyzer, audio thread | audio seam + editor overlay (relaxed atomic) | per block → `intensityEff` |
| **accent (new)** | analyzer, audio thread | audio seam + editor overlay (relaxed atomic) | per block → per-trigger velocity term |
| complexity | **APVTS only — Follow never writes it** | audio thread raw value, per block → per-trigger gate | immediate (unchanged) |
| intensityEff | computed inline, audio thread | `drummer.setIntensity` | immediate; smoothed by the macro envelope |
| followAccent / depth | audio thread (`setFollowAccent`) | `stepVelocity`, same thread | immediate, per trigger |
| followAmount | host/UI via APVTS | audio thread raw value | per block |
| bar latch | — | — | **not needed** — nothing Follow touches is structural (§6) |

- The analyzer→pattern hop is same-thread (all inside `processBlock`); the atomics exist for the editor's benefit, matching the existing `energy` idiom. `followAccent`/`followAccentDepth` in `DrumPattern` are plain floats like `intensity` — set and read on the audio thread only.
- Added audio-thread cost: one `std::exp`, two relaxed stores, a handful of arithmetic per block; one branch + fmadd per trigger. **No allocation, no locks, no `juce::Random`, no `juce::String`, no MessageManager** — all RT-safety rules hold.
- Determinism: Follow adds no hash draws, so seed/barIndex reproducibility is untouched. With Follow off (or depth 0) output is bit-identical to the pre-change build. With Follow on, output is a deterministic function of the guide audio — realtime == offline bounce *given the same guide*, the strongest guarantee any sidechain-driven effect can make.
- `SmoothedValue`: only `volumeSmoothed` exists today and is untouched; §5 explains why no new one is added.

---

## 10. Editor / XY pad while Follow is on

Today the pad is disabled and the puck is hijacked to the followed values. That presentation is now *wrong on both axes*: complexity never moves, and intensity is base+modulation, not a replacement. Specified new behavior:

1. **The pad stays fully enabled while Follow is on.** The grid is still authoritative, so the user must be able to keep steering both axes live — locking the pad would contradict requirement 1. The puck always renders the bound parameters (base position); Follow never writes the parameters (invariant preserved from the original doc — no automation feedback).
2. **The modulation is drawn as an overlay, not as puck movement**: while Follow is on and a guide is available, draw a hollow "follow ring" at `(puckX, intensityEff)` — same X as the puck, since complexity is never modulated — connected to the puck by a thin vertical line, in the existing `kAccent` color. The ring's radius (or stroke) pulses with `getGuideAccent()` — a cheap, legible "accent just landed" cue.
3. API change: **remove `XYPad::setDisplayPosition`** (its only caller was the Follow hijack) and add `void setFollowOverlay (bool active, float intensityEff01, float accent01)`; `updateFromProcessor()` feeds it from `getCurrentComplexity/Intensity/getGuideAccent` and stops calling `setEnabled(false)` for Follow. The editor's existing 10 Hz timer is adequate (the overlay is cosmetic; accent pulses last 100–300 ms so most register) — bumping to 20 Hz is an implementer's option, not a requirement.
4. Rejected alternative, for the record: keeping the pad disabled with a vertically-moving live puck. It hides the base position, blocks live complexity control during Follow, and misrepresents the new semantics (modulation would look like a value change).
5. Small additions: a "Follow Amount" slider next to the Follow button (dimmed when Follow is off, always attached for automation); update the stale doc comments (complexity is now *always* used, not "used when Follow is off"; `DrumPattern.h` flags comment; `EnergyAnalyzer.h` class comment; README Follow section).

---

## 11. Open design calls (flagged, not asserted)

1. **All tuning constants are by-ear placeholders**: macro 250/800 ms, margin 0.05, gain 2.5, neutral 0.25, regular-cell weight 0.4, ceilings 0.40/0.75, default amount 0.6. Tests must assert *direction and invariants only*, never exact values (roadmap Step 14's own rule). The most likely to move on listening: `kAccentWeightRegular` (0.4 may make busy-guide hats pump; the fallback is 0 = strict `kFlagAccent`-only, which is why it's one named constant).
2. **Fixed-ms accent window vs. tempo-scaled**: above ~180 BPM the accent decay (~250–300 ms) can bleed across two 16ths. v1 keeps constants fixed (the analyzer doesn't know the tempo); if audible, pass BPM into the analyzer and scale the macro attack — mechanical change.
3. **Blend ceiling 0.75 vs 1.0** at `followAmount = 1` (§5). Grid-keeps-authority was chosen; "locks tightly" purists may want 1.0.
4. **RMS-based fast envelope as the accent source**: adequate for bass/guitar-like guides; a per-block *peak* channel would sharpen detection for very percussive guides at large block sizes. Deferred — the two-envelope structure accepts a better fast detector as a drop-in later.
5. **Ghost promotion** (a strong guide accent converting a ghost into a full hit) is velocity-only and thus *legal* under the constraint, but it changes authored character; explicitly out of v1.

---

## 12. Roadmap steps (in the house format)

### Step 20 — Grid-preserving Follow core: accent + macro signals, velocity modulation, new seam

**Goal.** Retire the density-collapse Follow path with the modulation-layer design; Follow never touches complexity. Follow depth is an internal constant (0.6) this step.

**Files.** `src/drummer/EnergyAnalyzer.h/.cpp` (macro env, accent, delete density machine); `src/drummer/DrumPattern.h/.cpp` (`setFollowAccent`, accent term in `stepVelocity`, constants, `kFlagAccent` authoring for Jazz/Electronic, flags-comment fix); `src/drummer/AdaptiveDrummer.h/.cpp` (passthrough); `src/PluginProcessor.h/.cpp` (seam, `getGuideAccent()`, header comment); `tests/EnergyAnalyzerTest.cpp`, `tests/DrumPatternTest.cpp`, `tests/AdaptiveDrummerProcessorTest.cpp`.

### Step 21 — `followAmount` parameter + XY-pad follow overlay

**Goal.** Replace the internal 0.6 constant with the APVTS param; new pad presentation (§10).

**Files.** `src/PluginProcessor.cpp` (param, hint 3, seam reads it); `src/XYPad.h/.cpp` (remove `setDisplayPosition`, add `setFollowOverlay`); `src/PluginEditor.h/.cpp` (enabled pad, overlay feed, amount slider, layout); `tests/AdaptiveDrummerProcessorTest.cpp`. Gate: full pluginval strictness-10 on both platforms (parameter added).

---

*Summary of what changed vs. the original §3.4: Follow no longer maps energy→intensity and onset-rate→complexity as replacements. Complexity is permanently manual; the followed-complexity bar-latch, dead-band, and onset-rate window are deleted from the plan; the analyzer's 3-step density machine is deleted from the code; Follow becomes two bounded velocity-domain modulations — a two-envelope accent signal targeting `kFlagAccent`-weighted cells (the first runtime use of `StepCell::flags`), and a macro-energy lerp around the grid's base intensity — both scaled by one new `followAmount` parameter, with the XY pad remaining live and authoritative throughout.*
