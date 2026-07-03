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
 * Built as an audio effect / generator: it outputs drums. The "complexity"
 * parameter (which cells fire) is ALWAYS the grid's — Follow never touches
 * it. When "follow" is on and a guide is connected, it modulates the
 * effective intensity (bounded blend toward the guide's phrase-level energy,
 * around the grid's base "intensity") and adds a velocity-only accent term
 * at step boundaries (see DrumPattern::setFollowAccent) — a modulation layer
 * on top of the grid, not a replacement of it. The guide is a user-chosen
 * *other* track routed into the (disabled-by-default) Sidechain bus when
 * connected; otherwise it falls back to this effect's own input; with no
 * guide connected at all, Follow modulates nothing.
 *
 * Parameters (managed by apvts, not exhaustive):
 *   bpm         float  40-240  default 120   manual BPM (ignored when host provides tempo)
 *   style       choice Rock/Jazz/Electronic  default Rock
 *   intensity   float  0-1     default 0.55  dynamics axis — the grid's base; Follow modulates around it
 *   complexity  float  0-1     default 0.55  structural axis — always the grid's, Follow never touches it
 *   density     choice Sparse/Medium/Full    legacy; kept registered for parameter-ID
 *                                            stability, no longer read by processBlock()
 *                                            except to migrate a pre-2.0 saved session
 *   volume      float  0-1     default 0.8
 *   follow      bool           default off   guide-driven accent + intensity modulation on top of the grid
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

    /** How far the guide is currently accenting (0..1; see
        EnergyAnalyzer::getAccent). For the editor's Follow overlay. */
    float getGuideAccent () const noexcept { return energyAnalyzer.getAccent(); }

    /** True this block iff Follow is on AND a guide (Sidechain or main input)
        is actually connected -- i.e. iff Follow is modulating anything right
        now. For the editor's Follow overlay (show it only when there's
        something to show). */
    bool isGuideAvailable () const noexcept { return guideAvailableState.load (std::memory_order_relaxed); }

    /** The 2-D groove axes actually in use this block: complexity is always
        the grid's; intensity is the grid's base, modulated toward the guide
        when Follow is on and connected. Readable from the editor for the XY
        pad's live readout. */
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
    std::atomic<bool>   guideAvailableState    { false };

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> volumeSmoothed;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaptiveDrummerProcessor)
};
