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
 * When "follow" is on, the density follows the energy of the guide signal on
 * the sidechain input bus instead of the manual "density" parameter.
 *
 * Parameters (managed by apvts):
 *   bpm      float  40-240  default 120   manual BPM (ignored when host provides tempo)
 *   style    choice Rock/Jazz/Electronic  default Rock
 *   density  choice Sparse/Medium/Full    default Medium (used when Follow is off)
 *   volume   float  0-1     default 0.8
 *   follow   bool           default off   adaptive density from the guide track
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

    /** Smoothed guide-track energy in [0, 1] (for the editor meter). */
    float getEnergy () const noexcept { return energyAnalyzer.getEnergy(); }

    /** Density actually in use this block (manual, or adaptive when Follow is on). */
    DrumPattern::Density getCurrentDensity () const noexcept
    {
        return static_cast<DrumPattern::Density> (
            currentDensityState.load (std::memory_order_relaxed));
    }

    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    void autoLoadSamples();

    AdaptiveDrummer  drummer;
    EnergyAnalyzer   energyAnalyzer;
    double           currentBpm          { 120.0 };
    std::atomic<int> currentDensityState { static_cast<int> (DrumPattern::Density::Medium) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaptiveDrummerProcessor)
};
