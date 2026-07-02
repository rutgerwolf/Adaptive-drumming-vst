#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

/**
 * AdaptiveDrummerEditor
 *
 * Dark UI inspired by GarageBand Drummer:
 *   - Style selector  (Rock / Jazz / Electronic)
 *   - Intensity / Complexity sliders (the 2-D groove axes; a visual XY pad is
 *     a later UI pass — these are plain sliders for now)
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

    // The 2-D groove axes. Disabled (and shown live) while Follow is on, same
    // as the density buttons they replaced.
    juce::Slider intensitySlider, complexitySlider;
    juce::Label  intensityTitleLabel, complexityTitleLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> intensityAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> complexityAttachment;

    // Follow (adaptive density) + guide-energy meter
    juce::TextButton     followButton { "Follow" };
    juce::Rectangle<int> energyMeterBounds;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> followAttachment;

    // Sound source (synthesised voices vs. WAV samples)
    juce::TextButton sourceSynthButton   { "Synth" };
    juce::TextButton sourceSamplesButton { "Samples" };

    // Transport — drives the standalone; also overrides a stopped host transport
    juce::TextButton playButton { "Play" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> playAttachment;

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
