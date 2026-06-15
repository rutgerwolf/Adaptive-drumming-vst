#include "PluginProcessor.h"
#include "PluginEditor.h"

// Where the last successfully-loaded sample folder is remembered in plugin state.
static const juce::Identifier kSamplesPathId { "samplesPath" };

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

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "follow", 1 }, "Follow", false));

    return layout;
}

// ── Constructor / destructor ──────────────────────────────────────────────────

AdaptiveDrummerProcessor::AdaptiveDrummerProcessor()
    : AudioProcessor (BusesProperties()
          .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "AdaptiveDrummer", createParameterLayout())
{}

AdaptiveDrummerProcessor::~AdaptiveDrummerProcessor() = default;

// ── Bus layout ────────────────────────────────────────────────────────────────

bool AdaptiveDrummerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Main output must be stereo.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // The guide/sidechain input is optional: disabled, mono, or stereo.
    const auto sidechain = layouts.getChannelSet (true, 0);
    return sidechain.isDisabled()
        || sidechain == juce::AudioChannelSet::mono()
        || sidechain == juce::AudioChannelSet::stereo();
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void AdaptiveDrummerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    drummer.prepare (sampleRate, samplesPerBlock);
    energyAnalyzer.prepare (sampleRate, samplesPerBlock);

    autoLoadSamples();
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

    auto       mainOut    = getBusBuffer (buffer, false, 0);
    const int  numSamples = buffer.getNumSamples();
    const bool follow     = *apvts.getRawParameterValue ("follow") > 0.5f;

    // Analyse the guide/sidechain signal BEFORE clearing the buffer.
    if (follow)
    {
        if (auto* in = getBus (true, 0); in != nullptr && in->isEnabled())
            energyAnalyzer.processBlock (getBusBuffer (buffer, true, 0), numSamples);
        else
            energyAnalyzer.processSilence (numSamples);
    }
    else
    {
        energyAnalyzer.processSilence (numSamples);   // let the meter fall back
    }

    buffer.clear();   // safe now — the guide has already been analysed

    // Host transport: BPM takes priority over the manual parameter, and when the
    // transport is rolling the ppq position locks the drummer to the DAW timeline.
    double bpm         = static_cast<double> (*apvts.getRawParameterValue ("bpm"));
    bool   hostPlaying = false;
    double hostPpq     = 0.0;
    if (auto* playHead = getPlayHead())
        if (auto pos = playHead->getPosition())
        {
            if (auto hostBpm = pos->getBpm())
                bpm = *hostBpm;
            if (auto ppq = pos->getPpqPosition())
            {
                hostPpq     = *ppq;
                hostPlaying = pos->getIsPlaying();
            }
        }
    currentBpm = bpm;
    drummer.setBpm (bpm);
    drummer.setHostTimeline (hostPlaying, hostPpq);

    drummer.setStyle (static_cast<DrumPattern::Style> (
        static_cast<int> (*apvts.getRawParameterValue ("style"))));

    // Density: adaptive from the guide energy when Follow is on, else manual.
    const DrumPattern::Density density = follow
        ? energyAnalyzer.getDensity()
        : static_cast<DrumPattern::Density> (
              static_cast<int> (*apvts.getRawParameterValue ("density")));
    drummer.setDensity (density);
    currentDensityState.store (static_cast<int> (density), std::memory_order_relaxed);

    // Generate drums into the main output bus.
    drummer.processBlock (mainOut, numSamples);

    mainOut.applyGain (*apvts.getRawParameterValue ("volume"));
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
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));

            // Re-load the kit this session was saved with.
            const auto remembered = apvts.state.getProperty (kSamplesPathId).toString();
            if (remembered.isNotEmpty())
                drummer.loadSamples (juce::File (remembered));
        }
}

// ── Samples ───────────────────────────────────────────────────────────────────

bool AdaptiveDrummerProcessor::loadSamples (const juce::File& samplesRoot)
{
    const bool ok = drummer.loadSamples (samplesRoot);
    if (ok)
        apvts.state.setProperty (kSamplesPathId, samplesRoot.getFullPathName(), nullptr);
    return ok;
}

// Resolve and load a kit off the audio thread, without forcing the user to
// re-pick it every session: a folder remembered in saved state first, then
// assets shipped next to the plugin binary or the host/standalone executable.
void AdaptiveDrummerProcessor::autoLoadSamples()
{
    if (drummer.areSamplesLoaded())
        return;

    juce::Array<juce::File> candidates;

    const auto remembered = apvts.state.getProperty (kSamplesPathId).toString();
    if (remembered.isNotEmpty())
        candidates.add (juce::File (remembered));

    for (auto loc : { juce::File::currentExecutableFile,     // the plugin binary
                      juce::File::currentApplicationFile })   // the host / standalone
        candidates.add (juce::File::getSpecialLocation (loc)
                            .getParentDirectory()
                            .getChildFile ("assets/samples/salamander"));

    for (const auto& dir : candidates)
        if (dir.isDirectory() && drummer.loadSamples (dir))
        {
            apvts.state.setProperty (kSamplesPathId, dir.getFullPathName(), nullptr);
            return;
        }
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
