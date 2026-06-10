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
    : AudioProcessorEditor (&p), proc (p)
{
    setSize (420, 300);

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

    // ── Density buttons ────────────────────────────────────────────────────────
    for (auto* btn : { &sparseButton, &mediumButton, &fullButton })
    {
        btn->setRadioGroupId (2);
        btn->setClickingTogglesState (true);
        btn->setColour (juce::TextButton::buttonColourId,   kPanel);
        btn->setColour (juce::TextButton::buttonOnColourId, kActive);
        btn->setColour (juce::TextButton::textColourOffId,  kMuted);
        btn->setColour (juce::TextButton::textColourOnId,   kText);
        addAndMakeVisible (btn);
    }
    sparseButton.onClick = [this] { proc.apvts.getParameter ("density")->setValueNotifyingHost (
                                        proc.apvts.getParameter ("density")->convertTo0to1 (0.0f)); };
    mediumButton.onClick = [this] { proc.apvts.getParameter ("density")->setValueNotifyingHost (
                                        proc.apvts.getParameter ("density")->convertTo0to1 (1.0f)); };
    fullButton.onClick   = [this] { proc.apvts.getParameter ("density")->setValueNotifyingHost (
                                        proc.apvts.getParameter ("density")->convertTo0to1 (2.0f)); };

    // ── BPM display ────────────────────────────────────────────────────────────
    bpmTitleLabel.setText ("BPM", juce::dontSendNotification);
    bpmTitleLabel.setColour (juce::Label::textColourId, kMuted);
    bpmTitleLabel.setFont (juce::Font (11.0f));
    addAndMakeVisible (bpmTitleLabel);

    bpmValueLabel.setColour (juce::Label::textColourId,       kText);
    bpmValueLabel.setColour (juce::Label::backgroundColourId, kPanel);
    bpmValueLabel.setJustificationType (juce::Justification::centred);
    bpmValueLabel.setFont (juce::Font (20.0f, juce::Font::bold));
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
    g.drawText ("STYLE",   12, 36, 60, 12, juce::Justification::left);
    g.drawText ("DENSITY", 12, 94, 60, 12, juce::Justification::left);
}

// ── Layout ────────────────────────────────────────────────────────────────────

void AdaptiveDrummerEditor::resized()
{
    const int W = getWidth();
    const int m = 10;
    const int btnH = 28;
    const int btnW = (W - 4 * m) / 3;

    // Style buttons  y=50
    rockButton      .setBounds (m,                   50, btnW, btnH);
    jazzButton      .setBounds (m + btnW + m,        50, btnW, btnH);
    electronicButton.setBounds (m + 2 * (btnW + m),  50, btnW, btnH);

    // Density buttons  y=106
    sparseButton.setBounds (m,                   106, btnW, btnH);
    mediumButton.setBounds (m + btnW + m,        106, btnW, btnH);
    fullButton  .setBounds (m + 2 * (btnW + m),  106, btnW, btnH);

    // BPM  y=152
    bpmTitleLabel.setBounds (m,      152, 30, 14);
    bpmValueLabel.setBounds (m + 30, 148, 80, 28);

    // Volume  y=148 (right side)
    volumeTitleLabel.setBounds (W - m - 60 - 14, 152, 30, 14);
    volumeSlider    .setBounds (W - m - 60,       140, 60, 60);

    // Samples (bottom)
    loadSamplesButton  .setBounds (m,        260, 120, 24);
    samplesStatusLabel .setBounds (m + 128,  262, W - 140, 20);
}

// ── Timer ─────────────────────────────────────────────────────────────────────

void AdaptiveDrummerEditor::timerCallback()
{
    updateFromProcessor();
}

void AdaptiveDrummerEditor::updateFromProcessor()
{
    // BPM display
    bpmValueLabel.setText (
        juce::String (proc.getCurrentBpm(), 1),
        juce::dontSendNotification);

    // Sync style button toggle states from parameter (handles automation)
    const int style = static_cast<int> (*proc.apvts.getRawParameterValue ("style"));
    rockButton      .setToggleState (style == 0, juce::dontSendNotification);
    jazzButton      .setToggleState (style == 1, juce::dontSendNotification);
    electronicButton.setToggleState (style == 2, juce::dontSendNotification);

    // Sync density button toggle states
    const int density = static_cast<int> (*proc.apvts.getRawParameterValue ("density"));
    sparseButton.setToggleState (density == 0, juce::dontSendNotification);
    mediumButton.setToggleState (density == 1, juce::dontSendNotification);
    fullButton  .setToggleState (density == 2, juce::dontSendNotification);

    // Samples status
    const bool loaded = proc.areSamplesLoaded();
    samplesStatusLabel.setText (
        loaded ? "Samples loaded" : "No samples — click to load",
        juce::dontSendNotification);
    samplesStatusLabel.setColour (
        juce::Label::textColourId,
        loaded ? kText : juce::Colours::orange);
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
