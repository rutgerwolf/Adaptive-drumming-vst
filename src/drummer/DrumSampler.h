#pragma once
#include <JuceHeader.h>
#include "DrumPattern.h"

#include <atomic>

/**
 * DrumSampler — loads WAV samples and renders drum hits into an audio buffer.
 *
 * Expected directory layout (one WAV per subfolder is enough):
 *   <root>/kick/   <root>/snare/  <root>/hihat/
 *   <root>/crash/  <root>/ride/   <root>/tom/
 *
 * Compatible with the Salamander Drumkit and similar sample packs.
 * Missing voices produce silence; playback continues for loaded voices.
 *
 * Thread-safety: loadSamples() builds a new sample set on the calling
 * (message) thread and swaps it in under a SpinLock that processBlock()
 * try-locks on the audio thread, so loading never races the audio callback.
 *
 * Polyphony: each voice has a fixed pool of kSlotsPerVoice playback slots, so
 * up to that many overlapping hits of the same voice — two triggers inside one
 * process block, or a retrigger while the previous hit is still ringing — can
 * play back simultaneously instead of the newer hit overwriting/clicking over
 * an in-flight one. triggerVoice() claims a free slot, or steals the
 * most-advanced one (largest playPos) if the pool is full.
 */
class DrumSampler
{
public:
    DrumSampler();
    ~DrumSampler() = default;

    void prepare (double sampleRate);
    bool loadSamples (const juce::File& salamanderRoot);
    bool areSamplesLoaded() const noexcept { return anyVoiceLoaded.load (std::memory_order_relaxed); }

    void processBlock (juce::AudioBuffer<float>& outBuffer,
                       int                       numSamples,
                       double                    sampleRate,
                       const DrumPattern&        pattern,
                       double                    bpm,
                       int                       playheadSample,
                       uint32_t                  barIndex);

    void reset();

private:
    static constexpr int kNumVoices     = 6;
    static constexpr int kSlotsPerVoice = 4;
    static const char*   kVoiceDirs[kNumVoices];

    // Immutable-once-built bundle of voice samples. Swapped atomically under
    // sampleLock so the audio thread never reads a half-loaded buffer.
    struct SampleSet
    {
        juce::AudioBuffer<float> samples[kNumVoices];
        bool                     anyLoaded { false };
    };

    // Per-voice-slot playback cursor (audio thread only). Decoupled from sample
    // data so loading a new SampleSet doesn't touch playback state mid-block.
    struct Voice
    {
        int   playPos       { -1 };    // -1 = inactive
        int   triggerOffset { 0 };     // sample offset within the *current* block
        float gain          { 1.0f };  // per-hit velocity (0..1) from the groove table
    };

    bool loadFirstWavInDir (const juce::File& dir, juce::AudioBuffer<float>& dest);
    void triggerVoice      (int voiceIndex, int blockOffset, float velocity01);
    void mixVoices         (juce::AudioBuffer<float>& outBuffer, int numSamples);

    SampleSet      sampleSet;   // guarded by sampleLock
    juce::SpinLock sampleLock;  // brief: held only for the cheap swap
    Voice          voices[kNumVoices][kSlotsPerVoice];

    juce::AudioFormatManager formatManager;
    double                   currentSampleRate { 44100.0 };
    std::atomic<bool>        anyVoiceLoaded     { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumSampler)
};
