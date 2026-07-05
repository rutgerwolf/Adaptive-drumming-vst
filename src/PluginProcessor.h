#pragma once
#include <JuceHeader.h>
#include "drummer/AdaptiveDrummer.h"
#include "drummer/EnergyAnalyzer.h"

#include <atomic>

/**
 * AdaptiveDrummerProcessor
 *
 * JUCE AudioProcessor wrapping AdaptiveDrummer v1.
 * Reads host BPM from the DAW playhead; falls back to the "bpm" parameter.
 *
 * Built as an audio effect / generator: it outputs drums. When "follow" is
 * on and a guide is connected, it modulates BOTH groove axes: the effective
 * intensity and the effective complexity are bounded blends toward the
 * guide's phrase-level energy, around the grid's base values — one energy
 * signal moves "how loud" and "how busy" together (GarageBand/Logic
 * Drummer's combined behaviour), with the grid always keeping at least 25%
 * authority on each axis. Follow also adds a velocity-only accent term at
 * step boundaries (see DrumPattern::setFollowAccent) — a modulation layer
 * on top of the grid, not a replacement of it. The guide is a user-chosen
 * *other* track routed into the (enabled-by-default -- see the constructor
 * comment) Sidechain bus when connected; otherwise it falls back to this
 * effect's own input; with no guide connected at all, Follow modulates
 * nothing.
 *
 * Parameters (managed by apvts, not exhaustive):
 *   bpm         float  40-240  default 120   manual BPM (ignored when host provides tempo)
 *   style       choice Rock/Jazz/Electronic  default Rock
 *   intensity   float  0-1     default 0.55  dynamics axis — the grid's base; Follow modulates around it
 *   complexity  float  0-1     default 0.55  structural axis — the grid's base; Follow modulates around it
 *   density     choice Sparse/Medium/Full    legacy; kept registered for parameter-ID
 *                                            stability, no longer read by processBlock()
 *                                            except to migrate a pre-2.0 saved session
 *   volume      float  0-1     default 0.8
 *   follow      bool           default off   guide-driven accent + intensity/complexity modulation on top of the grid
 */
class AdaptiveDrummerProcessor : public juce::AudioProcessor,
                                 private juce::Timer
{
public:
    AdaptiveDrummerProcessor();
    ~AdaptiveDrummerProcessor() override;

    // ── AudioProcessor ─────────────────────────────────────────────────────────
    void prepareToPlay  (double sampleRate, int samplesPerBlock) override;
    void releaseResources () override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor () override;
    bool hasEditor () const override { return true; }

    const juce::String getName ()              const override { return "Adaptive Drummer"; }
    bool acceptsMidi ()                        const override { return false; }
    bool producesMidi ()                       const override { return false; }
    bool isMidiEffect ()                       const override { return false; }
    double getTailLengthSeconds ()             const override { return 1.5; }   // covers the ~1.2 s crash decay

    int  getNumPrograms ()                             override { return 1; }
    int  getCurrentProgram ()                          override { return 0; }
    void setCurrentProgram (int)                       override {}
    const juce::String getProgramName (int)            override { return "Default"; }
    void changeProgramName (int, const juce::String&)  override {}

    void getStateInformation (juce::MemoryBlock& data)              override;
    void setStateInformation (const void* data, int sizeInBytes)    override;

    // ── Plugin API ─────────────────────────────────────────────────────────────
    /** Synchronous load, called by the editor's "Load samples..." button (and
        by tests that just need a kit loaded deterministically). The async
        machinery below (Step 17) is only for lifecycle callbacks — this stays
        a direct, blocking call whose return value means "loaded right now". */
    bool loadSamples      (const juce::File& samplesRoot);
    bool areSamplesLoaded () const;

    /** Picks a new random groove seed (the "New Seed" dice button), persists
        it into state, and applies it immediately — a fresh set of ornament/
        humanization variations without changing style, complexity, or which
        cells fire. Non-automatable by design (not an AudioParameter): this is
        a "reshuffle", not a value a host would want to ramp/automate. */
    void     randomizeGrooveSeed();
    uint32_t getCurrentGrooveSeed() const noexcept { return currentGrooveSeed.load (std::memory_order_relaxed); }

    // ── Per-voice mutes (a TableExchange producer input) ──────────────────────
    /** Mutes/unmutes one drum voice (DrumPattern voice index: 0 Kick, 1 Snare,
        2 HiHat, 3 Crash, 4 Ride, 5 Tom). Message thread only (persists into
        apvts.state, like randomizeGrooveSeed) — the editor's mute buttons are
        the caller. Publishes the rebuilt table synchronously via
        publishDynamicTableIfNeeded(), so a click takes effect at the very next
        bar wrap. */
    void setVoiceMuted (int voice, bool shouldMute);
    bool isVoiceMuted  (int voice) const noexcept
    {
        return (muteMask.load (std::memory_order_relaxed) & (1u << voice)) != 0;
    }
    uint8_t getMuteMask () const noexcept { return muteMask.load (std::memory_order_relaxed); }

    // ── User-drawable groove grid (roadmap Step 19's destination) ─────────────
    /** True while a hand-drawn custom grid (not a built-in style) is the
        pattern source. Message thread only, like the rest of the grid API. */
    bool hasCustomGrid() const noexcept { return customGridActive; }

    /** The table the editor's grid displays and edits right now: the custom
        grid when one is active, else the target style's built-in table (keyed
        off the raw "style" parameter, exactly like the producer routine — the
        grid must show the style the user just *picked*, not the bar-latched
        one). Mutes are NOT baked into this table (they compose at publish
        time inside publishDynamicTableIfNeeded()); the editor dims muted rows
        via getMuteMask() instead. Message thread only. */
    const GrooveTable& getGridTable() const noexcept;

    /** Draws one grid cell (message thread only; the editor's GrooveGrid is
        the caller). The first edit enters "custom pattern" mode: the target
        style's built-in table is copied into the custom grid (so the drawing
        starts from what was audible) and every later edit mutates that same
        copy. Cell conventions (see the roadmap's Step 19 decisions):
          on  → { vel, vel, threshold 0, probability 255, rand 0, flags 0 } —
                a hand-placed hit always fires, at exactly the drawn velocity,
                regardless of the complexity axis or seed;
          off → StepCell{} (velHigh == 0 = the model's never-plays convention,
                the same zeroing the mute producer uses).
        velocity127 is clamped to [1, 127] for "on" (0 would contradict "on").
        Persists into apvts.state and publishes synchronously. */
    void setGridCell (int voice, int step, bool on, int velocity127);

    /** Exits custom-pattern mode: discards the drawn grid and republishes
        plain style + mutes (the editor's "Clear Grid" button). No-op when no
        custom grid is active. Message thread only. */
    void clearCustomGrid();

    /** The unified TableExchange producer routine (message thread ONLY — this
        is the mailbox's single producer): rebuilds the currently-intended
        table from (custom grid or target style's built-in table) + the mute
        mask, and publishes it if anything relevant changed since the last
        publish. Also owns the "a style pick exits custom-grid mode" rule
        (see the implementation comment). Idempotent — a no-change call
        returns immediately. Called synchronously by setVoiceMuted() /
        setGridCell() / clearCustomGrid() and from the processor's 20 Hz
        message-thread timer (see timerCallback() for why a timer, and why it
        polls the "style" *parameter* rather than the drummer's bar-latched
        style). Public so tests can drive it deterministically without pumping
        the timer. */
    void publishDynamicTableIfNeeded();

    /** The table actually driving the drummer right now (mirrors
        AdaptiveDrummer::getActiveTable()). Introspection for tests — pointer
        identity distinguishes a built-in style table from an adopted
        TableExchange slot. */
    const GrooveTable& getActiveGrooveTable () const noexcept { return drummer.getActiveTable(); }

    /** True while a background kit-load job is queued or running (Step 17).
        Introspection for tests; not needed by production callers. */
    bool isKitLoadInFlight () const noexcept { return kitLoadInFlight.load(); }

    /** How many background kit-load jobs have been requested so far (Step 17).
        Test hook: pins that repeated prepareToPlay calls with no kit found
        don't re-dispatch a scan every prepare. A counter (rather than a timing
        proxy) because dispatching is deliberately cheap in both the correct
        and the broken world — the work happens on the pool thread — so wall
        clock can't distinguish "re-dispatched" from "didn't". */
    int getKitLoadsRequested () const noexcept { return kitLoadsRequested.load(); }

    /** BPM currently in use (host-synced or manual). Readable from the editor. */
    double getCurrentBpm () const noexcept { return currentBpm.load (std::memory_order_relaxed); }

    /** True when the host is supplying the tempo (so the manual BPM has no effect). */
    bool isBpmFromHost () const noexcept { return bpmFromHost.load (std::memory_order_relaxed); }

    /** Smoothed guide-track energy in [0, 1] (for the editor meter). */
    float getEnergy () const noexcept { return energyAnalyzer.getEnergy(); }

    /** How far the guide is currently accenting (0..1; see
        EnergyAnalyzer::getAccent). For the editor's Follow overlay. */
    float getGuideAccent () const noexcept { return energyAnalyzer.getAccent(); }

    /** True this block iff Follow is on AND a guide (Sidechain or main input)
        is actually connected -- i.e. iff Follow is modulating anything right
        now. For the editor's Follow overlay (show it only when there's
        something to show). */
    bool isGuideAvailable () const noexcept { return guideAvailableState.load (std::memory_order_relaxed); }

    /** The 2-D groove axes actually in use this block: both are the grid's
        base values, modulated toward the guide's macro energy when Follow is
        on and connected (bounded — the grid keeps >= 25% weight on each
        axis; exact manual passthrough otherwise). Readable from the editor
        for the XY pad's live readout / Follow overlay. */
    float getCurrentComplexity () const noexcept { return currentComplexityState.load (std::memory_order_relaxed); }
    float getCurrentIntensity  () const noexcept { return currentIntensityState.load  (std::memory_order_relaxed); }

    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    /** The message-thread poll driving publishDynamicTableIfNeeded() (20 Hz).
        Why a processor-side timer, and why it exists at all — the style-flip
        race (see publishDynamicTableIfNeeded()'s implementation comment): a
        style change must cause a freshly-muted copy of the *new* style's
        table to reach the mailbox before that flip's own bar wrap. The
        editor's 10 Hz poll can't be the mechanism (the editor may be closed
        during host automation), and an APVTS parameterChanged listener can't
        publish directly (host automation may deliver it on the audio thread,
        where the producer side of TableExchange must never run — and
        triggerAsyncUpdate() posts a system message, which can block there
        too). So the processor polls: 20 Hz ⇒ ≤50 ms reaction, comfortably
        inside the shortest possible bar (1 s at the 240 BPM clamp). */
    void timerCallback() override;

    /** Decides whether an automatic kit load is due and, if so, dispatches it
        to the background pool (Step 17) — never loads inline. forceReload
        bypasses both the areSamplesLoaded() early-out and the triedAutoLoad
        one — used when the sample rate just changed, so a kit already loaded
        (and resampled) for the old rate gets reloaded and resampled for the
        new one. Call from the message thread only (reads apvts.state). */
    void autoLoadSamples (bool forceReload);

    /** Publishes `candidates` as the pending kit-load request and makes sure a
        pool job exists to consume it. Message thread only. If a job is already
        in flight it drains the new request itself (see runKitLoadJob) — no
        second job is ever dispatched while one is running. */
    void enqueueKitLoad (juce::Array<juce::File> candidates);

    /** The pool-thread job: drains pending candidate lists, trying each
        directory with drummer.loadSamples() until one succeeds. Never touches
        apvts from the pool thread — the success write-back is posted to the
        message thread via callAsync, guarded by weakThis. */
    void runKitLoadJob (juce::WeakReference<AdaptiveDrummerProcessor> weakThis);

    AdaptiveDrummer  drummer;
    EnergyAnalyzer   energyAnalyzer;
    bool             wasPlaying { false };   // audio-thread only; edge-detects transport stop
    double           lastPrepareSampleRate { 0.0 };   // detects a real sample-rate change across prepareToPlay calls
    std::atomic<double> currentBpm             { 120.0 };
    std::atomic<bool>   bpmFromHost            { false };
    std::atomic<float>  currentComplexityState { 0.55f };   // mask-equivalent to legacy Medium
    std::atomic<float>  currentIntensityState  { 0.55f };
    std::atomic<bool>   guideAvailableState    { false };

    // Groove seed (ornament/humanization hash input — see AdaptiveDrummer::setSeed).
    // Not an AudioParameter: a "reshuffle" trigger, not something a host should
    // automate/ramp. Persisted as a plain state-tree property (like
    // kSamplesPathId) so getStateInformation()/setStateInformation() carry it
    // across sessions for free (apvts.copyState()/replaceState() include it).
    std::atomic<uint32_t> currentGrooveSeed { DrumPattern::kDefaultGrooveSeed };

    // ── Per-voice mutes ────────────────────────────────────────────────────────
    // One bit per DrumPattern voice (bit 0 Kick … bit 5 Tom); set = muted.
    // Written from the message thread (setVoiceMuted / setStateInformation),
    // read anywhere. Persisted as a plain state-tree int property
    // (kMuteMaskId), like the groove seed — not an AudioParameter: a mixing
    // switch, not something a host should ramp/automate per-sample.
    std::atomic<uint8_t> muteMask { 0 };

    // ── User-drawable groove grid — message-thread producer state ─────────────
    // The custom grid's cells (valid iff customGridActive). Mutes are NOT
    // baked in here: they compose on top at publish time, so muting is
    // non-destructive to the drawing (unmuting restores the drawn row).
    // gridRevision bumps on every mutation/load, so the publish routine can
    // tell "same custom grid" from "same flag, different cells" (state
    // restore) without diffing 584 bytes.
    bool        customGridActive { false };
    GrooveTable customGrid {};
    uint32_t    gridRevision { 0 };

    // Style value last observed by the producer poll — the edge detector for
    // decision "a style pick exits custom-grid mode". Message thread only.
    int lastSeenStyleIdx { 0 };

    /** (Re)writes or removes the gridData/gridVersion state-tree properties to
        mirror customGridActive/customGrid. Message thread only. */
    void persistGridToState();

    // What publishDynamicTableIfNeeded() last published — message thread only.
    // Makes the 20 Hz poll idempotent (no mailbox spam when nothing changed)
    // and doubles as the retry latch: a tryStage() nullptr leaves these stale,
    // so the next tick simply tries again (the mailbox contract: coalesce and
    // retry, never spin). everPublished keeps a never-touched processor's
    // mailbox provably inert (nothing publishes until a mute or a grid edit
    // first needs to).
    uint8_t  lastPublishedMuteMask { 0 };
    int      lastPublishedStyle    { -1 };
    bool     lastPublishedCustom   { false };
    uint32_t lastPublishedGridRev  { 0 };
    bool     everPublished         { false };

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> volumeSmoothed;

    // ── Async kit loading (Step 17) ────────────────────────────────────────────
    // "Already scanned and found nothing" latch, so pluginval-style repeated
    // prepareToPlay calls don't re-dispatch the same doomed scan every prepare.
    // Also set by a successful explicit loadSamples(): the user's choice counts
    // as "tried", so a later prepare never races an unrelated auto-scan over
    // it (a genuine sample-rate change still reloads — via the remembered
    // path, which is that very kit). Message thread only.
    bool                    triedAutoLoad     { false };
    std::atomic<bool>       kitLoadInFlight   { false };   // a pool job is queued/running
    std::atomic<int>        kitLoadsRequested { 0 };       // see getKitLoadsRequested()
    juce::CriticalSection   pendingKitLock;
    juce::Array<juce::File> pendingKitCandidates;          // guarded by pendingKitLock
    bool                    hasPendingKitLoad { false };   // guarded by pendingKitLock

    // Declared after drummer (and last of the data members) so it is destroyed
    // first — its jobs capture `this` and call into drummer. The destructor
    // additionally joins jobs explicitly with a generous timeout; see there.
    juce::ThreadPool        kitLoadPool { 1 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (AdaptiveDrummerProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaptiveDrummerProcessor)
};
