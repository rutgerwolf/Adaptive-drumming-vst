#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

/**
 * AdaptiveDrummerEditor
 *
 * Dark UI inspired by GarageBand Drummer:
 *   - Style selector  (Rock / Jazz / Electronic)
 *   - Density selector (Sparse / Medium / Full)
 *   - BPM display (host-synced or manual)
 *   - Volume knob
 *   - Sample-load status + browse button
 */
class AdaptiveDrummerEditor : public juce::AudioProcessorEditor,
                               private juce::Timer
{
public:
    explicit AdaptiveDrummerEditor (AdaptiveDrummerProcessor& processor);
    ~AdaptiveDrummerEditor() override;

    void paint   (juce::Graphics& g) override;
    void resized () override;

private:
    void timerCallback    () override;
    void updateFromProcessor ();
    void openSampleFolder ();

    AdaptiveDrummerProcessor& proc;

    // Style row
    juce::TextButton rockButton       { "Rock" };
    juce::TextButton jazzButton       { "Jazz" };
    juce::TextButton electronicButton { "Electronic" };

    // Density row
    juce::TextButton sparseButton { "Sparse" };
    juce::TextButton mediumButton { "Medium" };
    juce::TextButton fullButton   { "Full" };

    // Follow (adaptive density) + guide-energy meter
    juce::TextButton     followButton { "Follow" };
    juce::Rectangle<int> energyMeterBounds;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> followAttachment;

    // Sound source (synthesised voices vs. WAV samples)
    juce::TextButton sourceSynthButton   { "Synth" };
    juce::TextButton sourceSamplesButton { "Samples" };

    // BPM
    juce::Label bpmTitleLabel;
    juce::Label bpmValueLabel;

    // Volume
    juce::Slider volumeSlider;
    juce::Label  volumeTitleLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> volumeAttachment;

    // Samples
    juce::TextButton loadSamplesButton  { "Load samples..." };
    juce::Label      samplesStatusLabel;
    std::unique_ptr<juce::FileChooser>  fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AdaptiveDrummerEditor)
};
