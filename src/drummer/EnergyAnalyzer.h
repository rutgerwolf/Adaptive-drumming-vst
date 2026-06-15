#pragma once
#include <JuceHeader.h>
#include "DrumPattern.h"

#include <atomic>

/**
 * EnergyAnalyzer — turns an incoming guide/sidechain signal into a smoothed
 * 0..1 "energy" value and an adaptive density.
 *
 * This is a fresh implementation written for the plugin's audio-thread context
 * (block-rate, lock-free reads for the UI); it is not lifted from any other
 * project.
 *
 * Pipeline per block:
 *   block RMS → dB → mapped to 0..1 → attack/release envelope → energy.
 * The energy then drives a 3-level density (Sparse/Medium/Full) through a
 * hysteresis band so it doesn't chatter between levels on small fluctuations.
 *
 * processBlock()/processSilence() are audio-thread only; getEnergy() and
 * getDensity() are safe to poll from the message thread (atomics).
 */
class EnergyAnalyzer
{
public:
    EnergyAnalyzer() = default;

    void prepare (double sampleRate, int blockSize);
    void reset();

    /** Analyse one block of guide audio (channels are summed to mono). */
    void processBlock (const juce::AudioBuffer<float>& guide, int numSamples);

    /** Advance the envelope toward silence (Follow on, but no guide signal). */
    void processSilence (int numSamples);

    /** Smoothed energy in [0, 1]; lock-free, for meters/UI. */
    float getEnergy() const noexcept { return energy.load (std::memory_order_relaxed); }

    /** Current adaptive density (with hysteresis applied). */
    DrumPattern::Density getDensity() const noexcept
    {
        return static_cast<DrumPattern::Density> (densityState.load (std::memory_order_relaxed));
    }

    /** Pure hysteresis mapping, exposed for testing. Returns the next density
        given the current one and a 0..1 energy. */
    static DrumPattern::Density nextDensity (DrumPattern::Density current, float energy) noexcept;

private:
    void updateEnvelope (float targetEnergy, int numSamples);

    // Energy mapping window (RMS in dB → 0..1).
    static constexpr float kMinDb = -48.0f;
    static constexpr float kMaxDb = -12.0f;

    // Hysteresis thresholds (rise high, fall lower → dead-band).
    static constexpr float kUpToMedium   = 0.30f;
    static constexpr float kDownToSparse = 0.22f;
    static constexpr float kUpToFull     = 0.66f;
    static constexpr float kDownToMedium = 0.58f;

    double sampleRate    { 44100.0 };
    float  attackTimeSec { 0.030f };
    float  releaseTimeSec{ 0.300f };

    float                envelopeLevel { 0.0f };  // audio thread only
    std::atomic<float>   energy        { 0.0f };
    std::atomic<int>     densityState  { static_cast<int> (DrumPattern::Density::Sparse) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnergyAnalyzer)
};
