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

    // The 2-D groove axes (DrumPattern.h). Version hint 2: added after v1,
    // some hosts key parameter identity on it. Defaults (0.55, 0.55) are
    // mask-equivalent to the legacy "density" default of Medium.
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "intensity", 2 }, "Intensity",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.55f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "complexity", 2 }, "Complexity",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.55f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "volume", 1 }, "Volume",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.8f));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "follow", 1 }, "Follow", false));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "source", 1 }, "Sound",
        juce::StringArray { "Synth", "Samples" }, 0));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "play", 1 }, "Play", false));

    return layout;
}

// ── Constructor / destructor ──────────────────────────────────────────────────

AdaptiveDrummerProcessor::AdaptiveDrummerProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "AdaptiveDrummer", createParameterLayout())
{}

AdaptiveDrummerProcessor::~AdaptiveDrummerProcessor() = default;

// ── Bus layout ────────────────────────────────────────────────────────────────

bool AdaptiveDrummerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    // In-place effect: the input (the host track / guide) matches the output,
    // or is absent for generator-only hosts.
    const auto in = layouts.getMainInputChannelSet();
    return in.isDisabled() || in == out;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void AdaptiveDrummerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    drummer.prepare (sampleRate, samplesPerBlock);
    energyAnalyzer.prepare (sampleRate, samplesPerBlock);

    volumeSmoothed.reset (sampleRate, 0.02);   // 20 ms ramp
    volumeSmoothed.setCurrentAndTargetValue (*apvts.getRawParameterValue ("volume"));

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

    auto       mainOut    = getBusBuffer (buffer, false, 0);  // in-place: holds the input on entry
    const int  numSamples = buffer.getNumSamples();
    const bool follow     = *apvts.getRawParameterValue ("follow") > 0.5f;

    // This is a generator effect: the host track feeding it is the guide. Analyse
    // that incoming audio BEFORE the buffer is overwritten with drums.
    if (follow)
        energyAnalyzer.processBlock (mainOut, numSamples);
    else
        energyAnalyzer.processSilence (numSamples);   // let the meter fall back

    buffer.clear();   // output is the generated drums (input is the guide, now analysed)

    // Host transport: BPM takes priority over the manual parameter, and when the
    // transport is rolling the ppq position locks the drummer to the DAW timeline.
    // Standalone has no meaningful transport, so we ignore its playhead there and
    // let the Play button drive playback instead.
    double bpm         = static_cast<double> (*apvts.getRawParameterValue ("bpm"));
    bool   fromHost    = false;
    bool   hostPlaying = false;
    bool   hostHasPpq  = false;
    double hostPpq     = 0.0;
    if (wrapperType != wrapperType_Standalone)
        if (auto* playHead = getPlayHead())
            if (auto pos = playHead->getPosition())
            {
                if (auto hostBpm = pos->getBpm())
                {
                    bpm      = *hostBpm;
                    fromHost = true;
                }
                // isPlaying is independent of whether ppq is also reported —
                // some hosts report one without the other, and treating "no
                // ppq" as "not playing" wrongly silenced playback.
                hostPlaying = pos->getIsPlaying();
                if (auto ppq = pos->getPpqPosition())
                {
                    hostPpq    = *ppq;
                    hostHasPpq = true;
                }
            }
    currentBpm  = bpm;
    bpmFromHost = fromHost;
    drummer.setBpm (bpm);
    drummer.setHostTimeline (hostPlaying, hostHasPpq, hostPpq);

    // Play when the user's Play toggle is on, or the host transport is rolling.
    const bool playing = *apvts.getRawParameterValue ("play") > 0.5f || hostPlaying;

    drummer.setStyle (static_cast<DrumPattern::Style> (
        static_cast<int> (*apvts.getRawParameterValue ("style"))));

    // Sound source: 0 = Synth (no samples needed), 1 = Samples.
    drummer.setUseSynth (static_cast<int> (*apvts.getRawParameterValue ("source")) == 0);

    // The 2-D groove axes: adaptive from the guide energy when Follow is on
    // (still driven by EnergyAnalyzer's 3-step hysteresis, mapped onto the
    // axes via the same legacy mapping the "density" parameter used), else
    // the manual Intensity/Complexity sliders directly.
    float complexity01, intensity01;
    if (follow)
        DrumPattern::mapLegacy (energyAnalyzer.getDensity(), complexity01, intensity01);
    else
    {
        complexity01 = *apvts.getRawParameterValue ("complexity");
        intensity01  = *apvts.getRawParameterValue ("intensity");
    }
    drummer.setComplexity (complexity01);
    drummer.setIntensity  (intensity01);
    currentComplexityState.store (complexity01, std::memory_order_relaxed);
    currentIntensityState.store  (intensity01,  std::memory_order_relaxed);

    // Generate drums into the main output bus (silence + rewind when stopped).
    if (playing)
        drummer.processBlock (mainOut, numSamples);
    else
        drummer.reset();

    // Volume — smoothed to avoid zipper noise when the knob moves (A3).
    volumeSmoothed.setTargetValue (*apvts.getRawParameterValue ("volume"));
    volumeSmoothed.applyGain (mainOut, numSamples);
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
            // A session saved before "intensity" existed has no <PARAM id="intensity".../>
            // child; detect that here, before replaceState() discards the original XML.
            const bool isPreGrooveTableState = xml->getChildByAttribute ("id", "intensity") == nullptr;

            apvts.replaceState (juce::ValueTree::fromXml (*xml));

            if (isPreGrooveTableState)
            {
                // Missing intensity/complexity left them at their built-in
                // default (0.55/0.55); derive the axes the old "density" this
                // session restored to actually meant, so the session's feel
                // survives the upgrade instead of silently resetting to Medium.
                const auto density = static_cast<DrumPattern::Density> (
                    static_cast<int> (*apvts.getRawParameterValue ("density")));
                float complexity01 = 0.0f, intensity01 = 0.0f;
                DrumPattern::mapLegacy (density, complexity01, intensity01);

                if (auto* p = apvts.getParameter ("complexity"))
                    p->setValueNotifyingHost (p->convertTo0to1 (complexity01));
                if (auto* p = apvts.getParameter ("intensity"))
                    p->setValueNotifyingHost (p->convertTo0to1 (intensity01));
            }

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
