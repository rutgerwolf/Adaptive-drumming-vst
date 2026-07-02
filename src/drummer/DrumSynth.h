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
 *
 * Polyphony: each voice has a fixed pool of kSlotsPerVoice playback slots, so
 * up to that many overlapping hits of the same voice — two triggers inside one
 * process block, or a retrigger while the previous hit is still ringing — can
 * sound simultaneously instead of the newer hit overwriting/clicking over an
 * in-flight one. triggerVoice() claims a free slot, or steals the most-decayed
 * one (largest t) if the pool is full.
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
                       int                       playheadSample,
                       uint32_t                  barIndex);

private:
    static constexpr int kNumVoices     = 6;
    static constexpr int kSlotsPerVoice = 4;

    struct Voice
    {
        bool   active      { false };
        double t           { 0.0 };   // seconds since trigger
        double phase       { 0.0 };   // oscillator phase (radians)
        int    startOffset { 0 };     // first in-block sample to render from
        float  noisePrev   { 0.0f };  // previous raw noise (for the HP differentiator)
        float  gain        { 1.0f };  // per-hit amplitude = velocity01^1.5 (perceptual curve)
    };

    void  triggerVoice  (int voiceIndex, int blockOffset, float velocity01);
    float renderVoice   (int voiceIndex, Voice& v);

    Voice        voices[kNumVoices][kSlotsPerVoice];
    double       sampleRate { 44100.0 };
    juce::Random rng;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumSynth)
};
