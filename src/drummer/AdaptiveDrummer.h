#pragma once
#include <JuceHeader.h>
#include "DrumPattern.h"
#include "DrumSampler.h"
#include "DrumSynth.h"

#include <atomic>

/**
 * AdaptiveDrummer
 *
 * Generates drum audio from a DrumPattern + DrumSampler.
 * Style and density are controlled externally (via plugin parameters).
 * BPM is provided by the DAW host or set manually.
 *
 * The in-pattern playhead always free-runs (advances by exactly the block
 * size each call), so consecutive blocks tile the pattern with no gap or
 * overlap. When the host is playing and reports a ppq position, the playhead
 * is gently corrected toward the ppq-derived position whenever it has
 * drifted more than a small tolerance, instead of being overwritten every
 * block — the previous every-block re-derivation, checked against a pattern
 * length that can't exactly equal the host's true (fractional-sample) bar
 * length, caused step boundaries to be scanned twice or not at all.
 *
 * Structural changes are bar-latched: setStyle() only records the request in
 * an atomic; processBlock() splits its render at bar wraps and applies the
 * pending style exactly when the playhead reaches step 0. Since each style is
 * an immutable const GrooveTable, applying it is a pointer re-point — calling
 * setStyle() every block (as the processor does) costs one relaxed atomic
 * store and rebuilds nothing (this dissolves review finding A2), and a style
 * flip can never restructure the groove mid-bar.
 *
 * A bar counter (barIndex) increments at every bar wrap and feeds the
 * pattern's stateless trigger hash, so probabilistic ornament cells vary per
 * bar but replay bit-exactly from reset for the same groove seed. It is a
 * local musical counter, not the host's bar number — a ppq re-lock or host
 * loop jump does not rewind it (only which ornaments play changes, never the
 * backbone).
 */
class AdaptiveDrummer
{
public:
    AdaptiveDrummer();

    void prepare (double sampleRate, int blockSize);
    void reset   ();

    /** Requests a style; the switch is applied at the next step-0 bar wrap
        (or immediately if the playhead is already at step 0 / just reset). */
    void setStyle   (DrumPattern::Style   style);
    void setBpm     (double bpm);
    void setDensity (DrumPattern::Density density);

    /** The 2-D groove axes (see DrumPattern.h). Applied immediately (not
        bar-latched) — only style switches are. */
    void setComplexity (float complexity01) noexcept;
    void setIntensity  (float intensity01)  noexcept;

    /** Choose the sound source: synthesised voices (no samples needed) or the
        WAV sampler. */
    void setUseSynth (bool shouldUseSynth) noexcept { useSynth = shouldUseSynth; }
    bool isUsingSynth() const noexcept { return useSynth; }

    /** Host transport state for the next block. When isPlaying and
        hasPpqPosition are both true, the free-running playhead is corrected
        toward ppqPosition once it drifts past a small tolerance; otherwise
        (no host, or a host that reports playing without a ppq position) the
        drummer simply free-runs. */
    void setHostTimeline (bool isPlaying, bool hasPpqPosition, double ppqPosition) noexcept;

    bool loadSamples    (const juce::File& salamanderRoot);
    bool areSamplesLoaded() const noexcept { return drumSampler.areSamplesLoaded(); }

    /** Add drum audio into outBuffer (caller must clear buffer first). */
    void processBlock (juce::AudioBuffer<float>& outBuffer, int numSamples);

    double               getBpm()     const noexcept { return bpm; }

    /** The style whose table is *active* (a pending setStyle() shows up here
        only once it has been applied at a bar wrap). */
    DrumPattern::Style   getStyle()   const noexcept { return drumPattern.getStyle(); }
    DrumPattern::Density getDensity() const noexcept { return drumPattern.getDensity(); }

    /** The in-pattern sample position used to render the block just
        processed. Exposed (read-only) for testing the free-run/host-resync
        behaviour; not needed by production callers. */
    int getCurrentPlayheadSample() const noexcept { return lastRenderedPlayhead; }

    /** Bars rendered since reset (feeds the groove hash). Exposed for tests. */
    uint32_t getBarIndex() const noexcept { return barIndex; }

    /** Map a host ppq position to a sample offset within a one-bar pattern.
        Exposed (and static) for testing. beatsPerBar matches the pattern length
        (4 quarter-note beats). Result is always in [0, patternLen). */
    static int ppqToPlayhead (double ppqPosition, int patternLenSamples,
                              double beatsPerBar = 4.0) noexcept;

private:
    /** Applies a pending style request (audio thread; called at step 0 only). */
    void applyPendingStyle() noexcept;

    /** Renders [startSample, startSample + numSamples) of outBuffer from
        in-pattern position posInPattern with the active engine. */
    void renderSegment (juce::AudioBuffer<float>& outBuffer,
                        int startSample, int numSamples, int posInPattern);

    DrumPattern drumPattern;
    DrumSampler drumSampler;
    DrumSynth   drumSynth;

    bool   useSynth          { true };   // default: audible without samples
    double bpm               { 120.0 };
    double currentSampleRate { 44100.0 };
    double playheadSample    { 0.0 };    // free-running; see processBlock()
    int    lastRenderedPlayhead { 0 };   // playhead used for the last-rendered block

    // Style requests land here (any thread) and are applied at the step-0 bar
    // wrap. release/acquire matches the future dynamic-table mailbox idiom;
    // for the built-in const tables relaxed would already be safe.
    std::atomic<int> pendingStyleIdx { static_cast<int> (DrumPattern::Style::Rock) };

    uint32_t barIndex { 0 };             // bars since reset; feeds the groove hash

    bool   hostIsPlaying     { false };
    bool   hostHasPpq        { false };
    double hostPpqPosition   { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaptiveDrummer)
};
