#include "PluginProcessor.h"
#include "PluginEditor.h"

// ── Parameter layout ──────────────────────────────────────────────────────────

juce::AudioProcessorValueTreeState::ParameterLayout
AdaptiveDrummerProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "bpm", 1 }, "BPM",
        juce::NormalisableRange<float> (40.0f, 240.0f, 0.1f), 120.0f));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "style", 1 }, "Style",
        juce::StringArray { "Rock", "Jazz", "Electronic" }, 0));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "density", 1 }, "Density",
        juce::StringArray { "Sparse", "Medium", "Full" }, 1));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "volume", 1 }, "Volume",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.8f));

    return layout;
}

// ── Constructor / destructor ──────────────────────────────────────────────────

AdaptiveDrummerProcessor::AdaptiveDrummerProcessor()
    : AudioProcessor (BusesProperties()
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "AdaptiveDrummer", createParameterLayout())
{}

AdaptiveDrummerProcessor::~AdaptiveDrummerProcessor() = default;

// ── Bus layout ────────────────────────────────────────────────────────────────

bool AdaptiveDrummerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void AdaptiveDrummerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    drummer.prepare (sampleRate, samplesPerBlock);

    // Auto-load samples from next to the executable (works for Standalone)
    if (! drummer.areSamplesLoaded())
    {
        const auto appDir = juce::File::getSpecialLocation (
            juce::File::currentApplicationFile).getParentDirectory();
        drummer.loadSamples (appDir.getChildFile ("assets/samples/salamander"));
    }
}

void AdaptiveDrummerProcessor::releaseResources()
{
    drummer.reset();
}

// ── Audio callback ────────────────────────────────────────────────────────────

void AdaptiveDrummerProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                             juce::MidiBuffer& /*midi*/)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    // BPM: host playhead takes priority over manual parameter
    double bpm = static_cast<double> (*apvts.getRawParameterValue ("bpm"));
    if (auto* playHead = getPlayHead())
        if (auto pos = playHead->getPosition())
            if (auto hostBpm = pos->getBpm())
                bpm = *hostBpm;
    currentBpm = bpm;
    drummer.setBpm (bpm);

    // Style and density from parameters
    drummer.setStyle (static_cast<DrumPattern::Style> (
        static_cast<int> (*apvts.getRawParameterValue ("style"))));
    drummer.setDensity (static_cast<DrumPattern::Density> (
        static_cast<int> (*apvts.getRawParameterValue ("density"))));

    // Generate drums
    drummer.processBlock (buffer, buffer.getNumSamples());

    // Volume
    buffer.applyGain (*apvts.getRawParameterValue ("volume"));
}

// ── State ─────────────────────────────────────────────────────────────────────

void AdaptiveDrummerProcessor::getStateInformation (juce::MemoryBlock& data)
{
    auto state = apvts.copyState();
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, data);
}

void AdaptiveDrummerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

// ── Samples ───────────────────────────────────────────────────────────────────

bool AdaptiveDrummerProcessor::loadSamples (const juce::File& samplesRoot)
{
    return drummer.loadSamples (samplesRoot);
}

bool AdaptiveDrummerProcessor::areSamplesLoaded () const
{
    return drummer.areSamplesLoaded();
}

// ── Editor ────────────────────────────────────────────────────────────────────

juce::AudioProcessorEditor* AdaptiveDrummerProcessor::createEditor()
{
    return new AdaptiveDrummerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AdaptiveDrummerProcessor();
}
