#pragma once
#include <JuceHeader.h>
#include "DrumPattern.h"

/**
 * DrumSampler — loads WAV samples and renders drum hits into an audio buffer.
 *
 * Expected directory layout (one WAV per subfolder is enough):
 *   <root>/kick/   <root>/snare/  <root>/hihat/
 *   <root>/crash/  <root>/ride/   <root>/tom/
 *
 * Compatible with the Salamander Drumkit and similar sample packs.
 * Missing voices produce silence; playback continues for loaded voices.
 */
class DrumSampler
{
public:
    DrumSampler();
    ~DrumSampler() = default;

    void prepare (double sampleRate);
    bool loadSamples (const juce::File& salamanderRoot);
    bool areSamplesLoaded() const noexcept { return anyVoiceLoaded; }

    void processBlock (juce::AudioBuffer<float>& outBuffer,
                       int                       numSamples,
                       double                    sampleRate,
                       const DrumPattern&        pattern,
                       double                    bpm,
                       int                       playheadSample);

    void reset();

private:
    struct Voice
    {
        juce::AudioBuffer<float> sample;
        int playPos       { -1 };  // -1 = inactive
        int triggerOffset { 0 };   // sample offset within current block
    };

    bool loadFirstWavInDir (const juce::File& dir, juce::AudioBuffer<float>& dest);
    void triggerVoice      (int voiceIndex, int blockOffset);
    void mixVoices         (juce::AudioBuffer<float>& outBuffer, int numSamples);

    static constexpr int kNumVoices = 6;
    static const char*   kVoiceDirs[kNumVoices];

    Voice voices[kNumVoices];

    juce::AudioFormatManager formatManager;
    double currentSampleRate { 44100.0 };
    bool   anyVoiceLoaded    { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumSampler)
};
