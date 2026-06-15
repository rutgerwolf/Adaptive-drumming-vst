#pragma once
#include <JuceHeader.h>
#include "DrumPattern.h"
#include "DrumSampler.h"

/**
 * AdaptiveDrummer
 *
 * Generates drum audio from a DrumPattern + DrumSampler.
 * Style and density are controlled externally (via plugin parameters).
 * BPM is provided by the DAW host or set manually.
 *
 * When the host transport is running, the in-pattern position is derived from
 * the host's ppq position each block (B3) so the drummer locks to the DAW bar
 * line; otherwise it free-runs from an internal sample counter (Standalone, or
 * a stopped transport).
 */
class AdaptiveDrummer
{
public:
    AdaptiveDrummer();

    void prepare (double sampleRate, int blockSize);
    void reset   ();

    void setStyle   (DrumPattern::Style   style);
    void setBpm     (double bpm);
    void setDensity (DrumPattern::Density density);

    /** Host transport state for the next block. When playing, the in-pattern
        position is taken from ppqPosition; otherwise the drummer free-runs. */
    void setHostTimeline (bool isPlaying, double ppqPosition) noexcept;

    bool loadSamples    (const juce::File& salamanderRoot);
    bool areSamplesLoaded() const noexcept { return drumSampler.areSamplesLoaded(); }

    /** Add drum audio into outBuffer (caller must clear buffer first). */
    void processBlock (juce::AudioBuffer<float>& outBuffer, int numSamples);

    double               getBpm()     const noexcept { return bpm; }
    DrumPattern::Style   getStyle()   const noexcept { return drumPattern.getStyle(); }
    DrumPattern::Density getDensity() const noexcept { return drumPattern.getDensity(); }

    /** Map a host ppq position to a sample offset within a one-bar pattern.
        Exposed (and static) for testing. beatsPerBar matches the pattern length
        (4 quarter-note beats). Result is always in [0, patternLen). */
    static int ppqToPlayhead (double ppqPosition, int patternLenSamples,
                              double beatsPerBar = 4.0) noexcept;

private:
    DrumPattern drumPattern;
    DrumSampler drumSampler;

    double bpm               { 120.0 };
    double currentSampleRate { 44100.0 };
    int    playheadSample    { 0 };

    bool   hostIsPlaying     { false };
    double hostPpqPosition   { 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaptiveDrummer)
};
