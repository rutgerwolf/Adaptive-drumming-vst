#pragma once
#include <JuceHeader.h>
#include "drummer/AdaptiveDrummer.h"

/**
 * AdaptiveDrummerProcessor
 *
 * JUCE AudioProcessor wrapping AdaptiveDrummer v1.
 * Reads host BPM from the DAW playhead; falls back to the "bpm" parameter.
 *
 * Parameters (managed by apvts):
 *   bpm      float  40-240  default 120   manual BPM (ignored when host provides tempo)
 *   style    choice Rock/Jazz/Electronic  default Rock
 *   density  choice Sparse/Medium/Full    default Medium
 *   volume   float  0-1     default 0.8
 */
class AdaptiveDrummerProcessor : public juce::AudioProcessor
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
    double getTailLengthSeconds ()             const override { return 0.0; }

    int  getNumPrograms ()                             override { return 1; }
    int  getCurrentProgram ()                          override { return 0; }
    void setCurrentProgram (int)                       override {}
    const juce::String getProgramName (int)            override { return "Default"; }
    void changeProgramName (int, const juce::String&)  override {}

    void getStateInformation (juce::MemoryBlock& data)              override;
    void setStateInformation (const void* data, int sizeInBytes)    override;

    // ── Plugin API ─────────────────────────────────────────────────────────────
    bool loadSamples      (const juce::File& samplesRoot);
    bool areSamplesLoaded () const;

    /** BPM currently in use (host-synced or manual). Readable from the editor. */
    double getCurrentBpm () const noexcept { return currentBpm; }

    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    AdaptiveDrummer drummer;
    double          currentBpm { 120.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaptiveDrummerProcessor)
};
