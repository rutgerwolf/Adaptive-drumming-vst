#include "PluginEditor.h"

// ── Colour palette ────────────────────────────────────────────────────────────
static const juce::Colour kBg     { 0xff0d0d14 };
static const juce::Colour kPanel  { 0xff16213e };
static const juce::Colour kActive { 0xff4a6fa5 };
static const juce::Colour kText   { 0xffe0e0e0 };
static const juce::Colour kMuted  { 0xff888899 };
static const juce::Colour kAccent { 0xff6c9bd1 };

// ── Constructor ───────────────────────────────────────────────────────────────

AdaptiveDrummerEditor::AdaptiveDrummerEditor (AdaptiveDrummerProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      groovePad (*p.apvts.getParameter ("complexity"), *p.apvts.getParameter ("intensity"),
                 "COMPLEXITY", "INTENSITY")
{
    setSize (420, 562);   // +146px vs. v2: a single square XY pad replaces both slider rows

    // ── Style buttons ──────────────────────────────────────────────────────────
    for (auto* btn : { &rockButton, &jazzButton, &electronicButton })
    {
        btn->setRadioGroupId (1);
        btn->setClickingTogglesState (true);
        btn->setColour (juce::TextButton::buttonColourId,   kPanel);
        btn->setColour (juce::TextButton::buttonOnColourId, kActive);
        btn->setColour (juce::TextButton::textColourOffId,  kMuted);
        btn->setColour (juce::TextButton::textColourOnId,   kText);
        addAndMakeVisible (btn);
    }
    rockButton.onClick       = [this] { proc.apvts.getParameter ("style")->setValueNotifyingHost (
                                            proc.apvts.getParameter ("style")->convertTo0to1 (0.0f)); };
    jazzButton.onClick       = [this] { proc.apvts.getParameter ("style")->setValueNotifyingHost (
                                            proc.apvts.getParameter ("style")->convertTo0to1 (1.0f)); };
    electronicButton.onClick = [this] { proc.apvts.getParameter ("style")->setValueNotifyingHost (
                                            proc.apvts.getParameter ("style")->convertTo0to1 (2.0f)); };

    // ── Groove XY pad (the 2-D groove axes) ──────────────────────────────────
    addAndMakeVisible (groovePad);

    // ── Follow toggle (adaptive density from the guide track) ────────────────
    followButton.setClickingTogglesState (true);
    followButton.setColour (juce::TextButton::buttonColourId,   kPanel);
    followButton.setColour (juce::TextButton::buttonOnColourId, kActive);
    followButton.setColour (juce::TextButton::textColourOffId,  kMuted);
    followButton.setColour (juce::TextButton::textColourOnId,   kText);
    addAndMakeVisible (followButton);
    followAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        proc.apvts, "follow", followButton);

    // ── Sound source buttons (Synth / Samples) ───────────────────────────────
    for (auto* btn : { &sourceSynthButton, &sourceSamplesButton })
    {
        btn->setRadioGroupId (3);
        btn->setClickingTogglesState (true);
        btn->setColour (juce::TextButton::buttonColourId,   kPanel);
        btn->setColour (juce::TextButton::buttonOnColourId, kActive);
        btn->setColour (juce::TextButton::textColourOffId,  kMuted);
        btn->setColour (juce::TextButton::textColourOnId,   kText);
        addAndMakeVisible (btn);
    }
    sourceSynthButton.onClick   = [this] { proc.apvts.getParameter ("source")->setValueNotifyingHost (
                                               proc.apvts.getParameter ("source")->convertTo0to1 (0.0f)); };
    sourceSamplesButton.onClick = [this] { proc.apvts.getParameter ("source")->setValueNotifyingHost (
                                               proc.apvts.getParameter ("source")->convertTo0to1 (1.0f)); };

    // ── Transport (Play / Stop) ──────────────────────────────────────────────
    playButton.setClickingTogglesState (true);
    playButton.setColour (juce::TextButton::buttonColourId,   kPanel);
    playButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff3fae6b));
    playButton.setColour (juce::TextButton::textColourOffId,  kText);
    playButton.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    addAndMakeVisible (playButton);
    playAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        proc.apvts, "play", playButton);

    // ── BPM display ────────────────────────────────────────────────────────────
    bpmTitleLabel.setText ("BPM", juce::dontSendNotification);
    bpmTitleLabel.setColour (juce::Label::textColourId, kMuted);
    bpmTitleLabel.setFont (juce::Font (11.0f));
    addAndMakeVisible (bpmTitleLabel);

    bpmValueLabel.setColour (juce::Label::textColourId,          kText);
    bpmValueLabel.setColour (juce::Label::backgroundColourId,    kPanel);
    bpmValueLabel.setColour (juce::Label::textWhenEditingColourId, kText);
    bpmValueLabel.setJustificationType (juce::Justification::centred);
    bpmValueLabel.setFont (juce::Font (20.0f, juce::Font::bold));
    bpmValueLabel.setEditable (false, true);   // double-click to type a tempo
    bpmValueLabel.onTextChange = [this]
    {
        if (auto* p = proc.apvts.getParameter ("bpm"))
        {
            const auto& range = p->getNormalisableRange();
            const float v     = range.snapToLegalValue (bpmValueLabel.getText().getFloatValue());
            p->setValueNotifyingHost (range.convertTo0to1 (v));
        }
    };
    addAndMakeVisible (bpmValueLabel);

    // ── Volume ──────────────────────────────────────────────────────────────────
    volumeTitleLabel.setText ("Vol", juce::dontSendNotification);
    volumeTitleLabel.setColour (juce::Label::textColourId, kMuted);
    volumeTitleLabel.setFont (juce::Font (11.0f));
    addAndMakeVisible (volumeTitleLabel);

    volumeSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    volumeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    volumeSlider.setColour (juce::Slider::rotarySliderFillColourId,     kAccent);
    volumeSlider.setColour (juce::Slider::rotarySliderOutlineColourId,  kPanel);
    volumeSlider.setColour (juce::Slider::thumbColourId,                kAccent);
    addAndMakeVisible (volumeSlider);

    volumeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        proc.apvts, "volume", volumeSlider);

    // ── Samples ────────────────────────────────────────────────────────────────
    samplesStatusLabel.setFont (juce::Font (11.0f));
    samplesStatusLabel.setColour (juce::Label::textColourId, kMuted);
    addAndMakeVisible (samplesStatusLabel);

    loadSamplesButton.setColour (juce::TextButton::buttonColourId,  kPanel);
    loadSamplesButton.setColour (juce::TextButton::textColourOffId, kAccent);
    loadSamplesButton.onClick = [this] { openSampleFolder(); };
    addAndMakeVisible (loadSamplesButton);

    updateFromProcessor();
    startTimerHz (10);
}

AdaptiveDrummerEditor::~AdaptiveDrummerEditor()
{
    stopTimer();
}

// ── Paint ─────────────────────────────────────────────────────────────────────

void AdaptiveDrummerEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);

    // Title bar
    g.setColour (kPanel);
    g.fillRect (0, 0, getWidth(), 30);
    g.setColour (kText);
    g.setFont (juce::Font (13.0f, juce::Font::bold));
    g.drawText ("ADAPTIVE DRUMMER", 0, 0, getWidth(), 30, juce::Justification::centred);

    // Section labels
    g.setFont (juce::Font (10.0f));
    g.setColour (kMuted);
    g.drawText ("STYLE",   12, 36,  60, 12, juce::Justification::left);
    g.drawText ("GROOVE",  12, 94,  60, 12, juce::Justification::left);
    g.drawText ("FOLLOW",  12, 322, 60, 12, juce::Justification::left);
    g.drawText ("ENERGY",  energyMeterBounds.getX(), 322, 80, 12, juce::Justification::left);
    g.drawText ("SOUND",   12, 378, 60, 12, juce::Justification::left);

    // Guide-energy meter
    g.setColour (kPanel);
    g.fillRect (energyMeterBounds);
    const float e = juce::jlimit (0.0f, 1.0f, proc.getEnergy());
    if (e > 0.0f)
    {
        auto fill = energyMeterBounds.toFloat()
                        .withWidth (energyMeterBounds.getWidth() * e)
                        .reduced (1.0f);
        g.setColour (kAccent);
        g.fillRect (fill);
    }
    g.setColour (kMuted);
    g.drawRect (energyMeterBounds);
}

// ── Layout ────────────────────────────────────────────────────────────────────

void AdaptiveDrummerEditor::resized()
{
    const int W = getWidth();
    const int m = 10;
    const int btnH = 28;
    const int btnW = (W - 4 * m) / 3;

    // Transport — top-right of the title bar
    playButton.setBounds (W - m - 72, 4, 72, 22);

    // Style buttons  y=50
    rockButton      .setBounds (m,                   50, btnW, btnH);
    jazzButton      .setBounds (m + btnW + m,        50, btnW, btnH);
    electronicButton.setBounds (m + 2 * (btnW + m),  50, btnW, btnH);

    // Groove XY pad  y=108-308 (replaces both slider rows with one square pad,
    // centred in the usable width; everything below shifts down 146px)
    constexpr int padSize = 200;
    const int padX = m + ((W - 2 * m) - padSize) / 2;   // centred in the usable width
    groovePad.setBounds (padX, 108, padSize, padSize);

    // Follow toggle + energy meter  y=336
    followButton.setBounds (m, 336, btnW, btnH);
    energyMeterBounds = juce::Rectangle<int> (m + btnW + m, 336, 2 * btnW + m, btnH);

    // Sound source buttons  y=392
    sourceSynthButton  .setBounds (m,            392, btnW, btnH);
    sourceSamplesButton.setBounds (m + btnW + m, 392, btnW, btnH);

    // BPM  y=444
    bpmTitleLabel.setBounds (m,      444, 30, 14);
    bpmValueLabel.setBounds (m + 30, 440, 80, 28);

    // Volume  (right side)
    volumeTitleLabel.setBounds (W - m - 60 - 14, 444, 30, 14);
    volumeSlider    .setBounds (W - m - 60,       432, 60, 60);

    // Samples (bottom)
    loadSamplesButton  .setBounds (m,        522, 120, 24);
    samplesStatusLabel .setBounds (m + 128,  524, W - 140, 20);
}

// ── Timer ─────────────────────────────────────────────────────────────────────

void AdaptiveDrummerEditor::timerCallback()
{
    updateFromProcessor();
    repaint (energyMeterBounds);   // live guide-energy meter
}

void AdaptiveDrummerEditor::updateFromProcessor()
{
    // Transport button reflects play state
    const bool playing = *proc.apvts.getRawParameterValue ("play") > 0.5f;
    playButton.setButtonText (playing ? "Stop" : "Play");

    // BPM: show the live tempo and allow typing it — except when the host is
    // driving the tempo (then the manual value would be ignored, so lock it).
    const bool bpmFromHost = proc.isBpmFromHost();
    if (! bpmValueLabel.isBeingEdited())
        bpmValueLabel.setText (juce::String (proc.getCurrentBpm(), 1),
                               juce::dontSendNotification);
    bpmValueLabel.setEditable (false, ! bpmFromHost);
    bpmValueLabel.setColour (juce::Label::textColourId, bpmFromHost ? kMuted : kText);

    // Sync style button toggle states from parameter (handles automation)
    const int style = static_cast<int> (*proc.apvts.getRawParameterValue ("style"));
    rockButton      .setToggleState (style == 0, juce::dontSendNotification);
    jazzButton      .setToggleState (style == 1, juce::dontSendNotification);
    electronicButton.setToggleState (style == 2, juce::dontSendNotification);

    // Groove pad: when Follow is on the axes are chosen by the guide energy,
    // so show the live adaptive position (a display-only move that doesn't
    // touch the bound parameters) and disable manual dragging — same pattern
    // the density buttons/sliders it replaced used.
    const bool following = *proc.apvts.getRawParameterValue ("follow") > 0.5f;
    if (following)
        groovePad.setDisplayPosition (proc.getCurrentComplexity(), proc.getCurrentIntensity());
    groovePad.setEnabled (! following);

    // Sound source buttons
    const bool useSynth = static_cast<int> (*proc.apvts.getRawParameterValue ("source")) == 0;
    sourceSynthButton  .setToggleState (useSynth,   juce::dontSendNotification);
    sourceSamplesButton.setToggleState (! useSynth, juce::dontSendNotification);

    // Samples status — only a warning in Samples mode (Synth needs no WAVs).
    const bool loaded = proc.areSamplesLoaded();
    if (useSynth)
    {
        samplesStatusLabel.setText (loaded ? "Synth · samples ready" : "Synth · no samples needed",
                                    juce::dontSendNotification);
        samplesStatusLabel.setColour (juce::Label::textColourId, kMuted);
    }
    else
    {
        samplesStatusLabel.setText (loaded ? "Samples loaded" : "No samples — click to load",
                                    juce::dontSendNotification);
        samplesStatusLabel.setColour (juce::Label::textColourId,
                                      loaded ? kText : juce::Colours::orange);
    }
}

// ── Sample folder dialog ──────────────────────────────────────────────────────

void AdaptiveDrummerEditor::openSampleFolder()
{
    fileChooser = std::make_unique<juce::FileChooser> (
        "Select drum samples root folder",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory));

    fileChooser->launchAsync (
        juce::FileBrowserComponent::openMode |
        juce::FileBrowserComponent::canSelectDirectories,
        [this] (const juce::FileChooser& fc)
        {
            const auto results = fc.getResults();
            if (results.isEmpty()) return;

            if (! proc.loadSamples (results[0]))
                juce::AlertWindow::showMessageBoxAsync (
                    juce::MessageBoxIconType::WarningIcon,
                    "Samples not found",
                    "No drum samples found in the selected folder.\n\n"
                    "Expected subfolders: kick/  snare/  hihat/  crash/  ride/  tom/\n"
                    "Each folder needs at least one WAV file.\n\n"
                    "Compatible pack: Salamander Drumkit\n"
                    "(archive.org/details/SalamanderDrumkit)");
        });
}
