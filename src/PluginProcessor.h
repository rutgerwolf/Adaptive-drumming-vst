#pragma once
#include <JuceHeader.h>
#include "drummer/AdaptiveDrummer.h"
#include "drummer/EnergyAnalyzer.h"

#include <atomic>

/**
 * AdaptiveDrummerProcessor
 *
 * JUCE AudioProcessor wrapping AdaptiveDrummer v1.
 * Reads host BPM from the DAW playhead; falls back to the "bpm" parameter.
 *
 * Built as an audio effect / generator: it outputs drums, and when "follow" is on
 * the 2-D groove axes track the energy of the incoming audio (the host track /
 * guide) instead of the manual "intensity"/"complexity" parameters.
 *
 * Parameters (managed by apvts, not exhaustive):
 *   bpm         float  40-240  default 120   manual BPM (ignored when host provides tempo)
 *   style       choice Rock/Jazz/Electronic  default Rock
 *   intensity   float  0-1     default 0.55  dynamics axis (used when Follow is off)
 *   complexity  float  0-1     default 0.55  structural axis (used when Follow is off)
 *   density     choice Sparse/Medium/Full    legacy; kept registered for parameter-ID
 *                                            stability, no longer read by processBlock()
 *                                            except to migrate a pre-2.0 saved session
 *   volume      float  0-1     default 0.8
 *   follow      bool           default off   adaptive axes from the guide track
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
    double getCurrentBpm () const noexcept { return currentBpm.load (std::memory_order_relaxed); }

    /** True when the host is supplying the tempo (so the manual BPM has no effect). */
    bool isBpmFromHost () const noexcept { return bpmFromHost.load (std::memory_order_relaxed); }

    /** Smoothed guide-track energy in [0, 1] (for the editor meter). */
    float getEnergy () const noexcept { return energyAnalyzer.getEnergy(); }

    /** The 2-D groove axes actually in use this block (manual sliders, or
        adaptive — via the guide energy's legacy density mapped through
        DrumPattern::mapLegacy — when Follow is on). Readable from the editor
        so the Intensity/Complexity sliders can reflect Follow's live value
        while disabled, the same way the old density buttons did. */
    float getCurrentComplexity () const noexcept { return currentComplexityState.load (std::memory_order_relaxed); }
    float getCurrentIntensity  () const noexcept { return currentIntensityState.load  (std::memory_order_relaxed); }

    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    void autoLoadSamples();

    AdaptiveDrummer  drummer;
    EnergyAnalyzer   energyAnalyzer;
    std::atomic<double> currentBpm             { 120.0 };
    std::atomic<bool>   bpmFromHost            { false };
    std::atomic<float>  currentComplexityState { 0.55f };   // mask-equivalent to legacy Medium
    std::atomic<float>  currentIntensityState  { 0.55f };

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> volumeSmoothed;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaptiveDrummerProcessor)
};
