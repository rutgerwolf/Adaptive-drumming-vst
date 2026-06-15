#pragma once
#include <JuceHeader.h>
#include "DrumPattern.h"

/**
 * DrumSynth — a small procedural drum voice generator.
 *
 * An alternative sound source to DrumSampler that needs no WAV files, so the
 * plugin makes sound out of the box. Each of the six voices is synthesised:
 *   Kick  — pitch-swept sine + amp decay
 *   Snare — tonal body + enveloped noise
 *   HiHat — short bright noise burst
 *   Crash — long noise decay
 *   Ride  — noise wash + a tonal ping
 *   Tom   — pitch-swept sine
 *
 * Same processBlock contract as DrumSampler (adds into outBuffer), so
 * AdaptiveDrummer can swap between the two.
 */
class DrumSynth
{
public:
    DrumSynth() = default;

    void prepare (double sampleRate);
    void reset();

    void processBlock (juce::AudioBuffer<float>& outBuffer,
                       int                       numSamples,
                       double                    sampleRate,
                       const DrumPattern&        pattern,
                       double                    bpm,
                       int                       playheadSample);

private:
    static constexpr int kNumVoices = 6;

    struct Voice
    {
        bool   active      { false };
        double t           { 0.0 };   // seconds since trigger
        double phase       { 0.0 };   // oscillator phase (radians)
        int    startOffset { 0 };     // first in-block sample to render from
        float  noisePrev   { 0.0f };  // previous raw noise (for the HP differentiator)
    };

    void  triggerVoice  (int voiceIndex, int blockOffset);
    float renderVoice   (int voiceIndex, Voice& v);

    Voice        voices[kNumVoices];
    double       sampleRate { 44100.0 };
    juce::Random rng;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumSynth)
};
