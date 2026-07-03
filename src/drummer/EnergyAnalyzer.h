#pragma once
#include <JuceHeader.h>

#include <atomic>

/**
 * EnergyAnalyzer — turns an incoming guide signal into a smoothed 0..1
 * "energy" value plus two Follow-modulation signals, both consumed by
 * PluginProcessor's Follow seam (never by DrumPattern's complexity/structural
 * path, which Follow does not touch — see DrumPattern.h):
 *
 *   - macro energy: a slower envelope of the same signal (the phrase-level
 *     loudness contour), used to modulate the *base* intensity up/down.
 *   - accent: how far the fast envelope currently pokes above that slower
 *     contour — a transient/emphasis signal, used to modulate per-hit
 *     velocity at step boundaries.
 *
 * This is a fresh implementation written for the plugin's audio-thread context
 * (block-rate, lock-free reads for the UI); it is not lifted from any other
 * project.
 *
 * Pipeline per block:
 *   block RMS → dB → mapped to 0..1 (target) → fast envelope → energy
 *                                             └→ slower envelope → macroEnergy
 *   accent = clamp01((energy - macroEnergy - margin) * gain)
 *
 * processBlock()/processSilence() are audio-thread only; getEnergy(),
 * getMacroEnergy(), getAccent() are safe to poll from the message thread
 * (atomics).
 */
class EnergyAnalyzer
{
public:
    EnergyAnalyzer() = default;

    void prepare (double sampleRate, int blockSize);
    void reset();

    /** Analyse one block of guide audio (channels are summed to mono). */
    void processBlock (const juce::AudioBuffer<float>& guide, int numSamples);

    /** Advance the envelopes toward silence (Follow on, but no guide signal). */
    void processSilence (int numSamples);

    /** Smoothed energy in [0, 1]; lock-free, for meters/UI. */
    float getEnergy() const noexcept { return energy.load (std::memory_order_relaxed); }

    /** Slower "phrase-level" envelope of the same signal, in [0, 1]. */
    float getMacroEnergy() const noexcept { return macroEnergy.load (std::memory_order_relaxed); }

    /** How far the fast envelope currently pokes above macroEnergy, in [0, 1]
        (0 on a steady signal or silence; rises for ~0.1-0.3s after a transient). */
    float getAccent() const noexcept { return accent.load (std::memory_order_relaxed); }

private:
    void updateEnvelope (float targetEnergy, int numSamples);

    // Energy mapping window (RMS in dB → 0..1).
    static constexpr float kMinDb = -48.0f;
    static constexpr float kMaxDb = -12.0f;

    // Macro (phrase-level) envelope time constants — deliberately slower than
    // the fast envelope so per-note guide dynamics drive the accent channel,
    // not the intensity contour.
    static constexpr float kMacroAttackSec  = 0.250f;
    static constexpr float kMacroReleaseSec = 0.800f;

    // Accent = (fast - macro), gated past this margin (rejects envelope
    // jitter on a steady signal) and scaled by this gain.
    static constexpr float kAccentMargin = 0.05f;
    static constexpr float kAccentGain   = 2.5f;

    double sampleRate    { 44100.0 };
    float  attackTimeSec { 0.030f };
    float  releaseTimeSec{ 0.300f };

    float                envelopeLevel { 0.0f };  // audio thread only (fast envelope)
    float                macroLevel    { 0.0f };  // audio thread only (macro envelope)
    std::atomic<float>   energy        { 0.0f };
    std::atomic<float>   macroEnergy   { 0.0f };
    std::atomic<float>   accent        { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnergyAnalyzer)
};
