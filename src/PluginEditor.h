#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "XYPad.h"
#include "GrooveGrid.h"
#include "TapTempo.h"

/**
 * AdaptiveDrummerEditor
 *
 * Dark UI inspired by GarageBand Drummer:
 *   - Style selector  (Rock / Jazz / Electronic)
 *   - Groove XY pad (Complexity x, Intensity y — the 2-D groove axes)
 *   - Follow toggle + Follow Amount (guide-driven modulation strength)
 *   - Groove grid (6 voices x 16 steps, hand-drawable — roadmap Step 19)
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

    // The 2-D groove axes. Stays enabled and draggable while Follow is on --
    // the grid remains authoritative; Follow's modulation shows as an overlay
    // on the pad instead of hijacking the puck (see XYPad::setFollowOverlay).
    XYPad groovePad;

    // Reshuffles the groove's ornament/humanization seed (a fresh set of fills
    // and micro-timing variations) without touching style, complexity, or
    // which cells fire — see AdaptiveDrummerProcessor::randomizeGrooveSeed().
    juce::TextButton newSeedButton { "New Seed" };

    // Follow (guide-driven accent + intensity modulation) + guide-energy meter
    juce::TextButton     followButton { "Follow" };
    juce::Rectangle<int> energyMeterBounds;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> followAttachment;

    // How strongly Follow's modulation applies on top of the grid
    juce::Slider followAmountSlider;
    juce::Label  followAmountTitleLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> followAmountAttachment;

    // Sound source (synthesised voices vs. WAV samples)
    juce::TextButton sourceSynthButton   { "Synth" };
    juce::TextButton sourceSamplesButton { "Samples" };

    // Per-voice mute toggles (on = muted), one per DrumPattern voice in bit
    // order: Kick, Snare, HiHat, Crash, Ride, Tom. Clicking calls
    // AdaptiveDrummerProcessor::setVoiceMuted(), which publishes a muted copy
    // of the style table through the TableExchange mailbox — the change lands
    // at the next bar wrap. Toggle state is re-synced from the processor's
    // mask each timer tick (same pattern as the style/source buttons).
    juce::TextButton muteButtons[GrooveTable::kNumVoices];

    // User-drawable groove grid (roadmap Step 19). Polled from the processor's
    // getGridTable()/getMuteMask() at the editor's existing 10 Hz rate (same
    // pattern as the style/mute buttons); edits are forwarded to
    // AdaptiveDrummerProcessor::setGridCell(). "Clear Grid" drops back to
    // plain style + mutes (AdaptiveDrummerProcessor::clearCustomGrid()).
    GrooveGrid       grooveGrid;
    juce::TextButton clearGridButton { "Clear Grid" };

    // Transport — drives the standalone; also overrides a stopped host transport
    juce::TextButton playButton { "Play" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> playAttachment;

    // BPM
    juce::Label bpmTitleLabel;
    juce::Label bpmValueLabel;

    // Tap tempo — a calculator, not a host-tempo override: tapping sets our
    // own "bpm" parameter (exactly like typing a value into bpmValueLabel
    // does), which only drives playback when the host isn't already supplying
    // a tempo. tapResultLabel shows the just-tapped value even when
    // bpmValueLabel is locked to a host-driven tempo, so the user always has
    // a number to read off and enter into the DAW's own tempo field by hand —
    // no plugin format gives an effect a way to write the host's tempo itself.
    juce::TextButton tapTempoButton { "TAP" };
    juce::Label      tapResultLabel;
    juce::Array<double> tapTimestampsMs;
    void handleTapTempo();

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
