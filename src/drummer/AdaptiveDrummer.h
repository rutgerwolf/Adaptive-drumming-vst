#pragma once
#include <JuceHeader.h>
#include "DrumPattern.h"
#include "DrumSampler.h"

/**
 * AdaptiveDrummer v1
 *
 * Generates drum audio from a DrumPattern + DrumSampler.
 * Style and density are controlled externally (via plugin parameters).
 * BPM is provided by the DAW host or set manually.
 *
 * v2 will add guide-track analysis (Follow Rhythm feature).
 * v3 will add the 2-D complexity/energy matrix from GarageBand's XY pad.
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

    bool loadSamples    (const juce::File& salamanderRoot);
    bool areSamplesLoaded() const noexcept { return drumSampler.areSamplesLoaded(); }

    /** Add drum audio into outBuffer (caller must clear buffer first). */
    void processBlock (juce::AudioBuffer<float>& outBuffer, int numSamples);

    double               getBpm()     const noexcept { return bpm; }
    DrumPattern::Style   getStyle()   const noexcept { return drumPattern.getStyle(); }
    DrumPattern::Density getDensity() const noexcept { return drumPattern.getDensity(); }

private:
    DrumPattern drumPattern;
    DrumSampler drumSampler;

    double bpm               { 120.0 };
    double currentSampleRate { 44100.0 };
    int    playheadSample    { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaptiveDrummer)
};
