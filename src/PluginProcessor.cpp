#include "PluginProcessor.h"
#include "PluginEditor.h"

// Where the last successfully-loaded sample folder is remembered in plugin state.
static const juce::Identifier kSamplesPathId { "samplesPath" };

// Looks for the shipped Salamander manifest (see DrumSampler's class comment)
// next to the plugin binary, then next to the host/standalone executable —
// the same two-candidate search autoLoadSamples() uses for the kit itself.
static juce::File findShippedManifest()
{
    for (auto loc : { juce::File::currentExecutableFile,
                      juce::File::currentApplicationFile })
    {
        const auto candidate = juce::File::getSpecialLocation (loc)
                                    .getParentDirectory()
                                    .getChildFile ("assets/salamander_manifest.json");
        if (candidate.existsAsFile())
            return candidate;
    }

    return {};
}

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

    // How strongly Follow's guide-driven accent + intensity modulation applies
    // on top of the grid (see the Follow seam in processBlock()). Version hint
    // 3: added after intensity/complexity (hint 2). Default 0.6 matches the
    // fixed depth Follow shipped with before this parameter existed.
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "followAmount", 3 }, "Follow Amount",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.6f));

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
          .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "AdaptiveDrummer", createParameterLayout())
{}

AdaptiveDrummerProcessor::~AdaptiveDrummerProcessor()
{
    // Join (or cancel, if not yet started) any background kit load before the
    // members it touches — drummer above all — are torn down. A load is
    // bounded work (one directory scan + a handful of WAV reads), so give it
    // a generous timeout to finish rather than rely on ThreadPool's own
    // destructor, which only waits 5 s before stopThreads() forcibly kills
    // the thread mid-load. (kitLoadPool is also declared last, so even its
    // fallback teardown runs before drummer's.)
    kitLoadPool.removeAllJobs (true, 30000);
}

// ── Bus layout ────────────────────────────────────────────────────────────────

bool AdaptiveDrummerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    // In-place effect: the input (the host track / guide) matches the output,
    // or is absent for generator-only hosts.
    const auto in = layouts.getMainInputChannelSet();
    if (! (in.isDisabled() || in == out))
        return false;

    // Sidechain (the followed track): independently disabled, mono, or
    // stereo — never coupled to the main layout.
    if (layouts.inputBuses.size() > 1)
    {
        const auto sidechain = layouts.inputBuses[1];
        if (! (sidechain.isDisabled() || sidechain == juce::AudioChannelSet::mono()
               || sidechain == juce::AudioChannelSet::stereo()))
            return false;
    }

    return true;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void AdaptiveDrummerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // A kit already loaded (and resampled — see DrumSampler::readWavInto) for
    // the previous rate must be reloaded and resampled for this one, not left
    // as-is just because something is already loaded.
    const bool sampleRateChanged = ! juce::approximatelyEqual (sampleRate, lastPrepareSampleRate);
    lastPrepareSampleRate = sampleRate;

    drummer.prepare (sampleRate, samplesPerBlock);
    energyAnalyzer.prepare (sampleRate, samplesPerBlock);
    wasPlaying = false;

    volumeSmoothed.reset (sampleRate, 0.02);   // 20 ms ramp
    volumeSmoothed.setCurrentAndTargetValue (*apvts.getRawParameterValue ("volume"));

    autoLoadSamples (sampleRateChanged);
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
    bool       guideAvailable = false;

    // Guide priority: a connected Sidechain bus (a user-chosen *other* track,
    // routed in via the host's sidechain/send routing) wins; otherwise fall
    // back to this effect's own input (today's "monitor mode"); otherwise
    // there is no guide at all. Analyse BEFORE the buffer is overwritten with
    // drums — mainOut still holds the live input at this point.
    if (follow)
    {
        auto* sidechainBus = getBus (true, 1);
        const bool sidechainConnected = sidechainBus != nullptr && sidechainBus->isEnabled()
                                         && sidechainBus->getNumberOfChannels() > 0;

        if (sidechainConnected)
        {
            energyAnalyzer.processBlock (getBusBuffer (buffer, true, 1), numSamples);
            guideAvailable = true;
        }
        else if (auto* mainIn = getBus (true, 0); mainIn != nullptr && mainIn->isEnabled())
        {
            energyAnalyzer.processBlock (mainOut, numSamples);
            guideAvailable = true;
        }
        else
            energyAnalyzer.processSilence (numSamples);
    }
    else
        energyAnalyzer.processSilence (numSamples);   // let the meter fall back

    // Output is the generated drums. Clear only the main-out channels — never
    // the whole buffer, which would also wipe the Sidechain bus's channels
    // (some hosts alias buffers, and the Sidechain view above must stay valid
    // for the rest of this call).
    mainOut.clear();

    // Host transport: BPM takes priority over the manual parameter, and when the
    // transport is rolling the ppq position locks the drummer to the DAW timeline.
    // Standalone has no meaningful transport, so we ignore its playhead there and
    // let the Play button drive playback instead.
    double bpm           = static_cast<double> (*apvts.getRawParameterValue ("bpm"));
    bool   fromHost      = false;
    bool   hostPlaying   = false;
    bool   hostHasPpq    = false;
    double hostPpq       = 0.0;
    double hostBeatsPerBar = 4.0;
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
                // ppq is always in quarter notes regardless of the time
                // signature's denominator, so a host bar is
                // numerator * (4 / denominator) quarter-note beats long (e.g.
                // 3/4 and 6/8 both give 3.0) — this is what a ppq resync needs
                // to land on the host's actual bar line instead of always
                // assuming 4/4 (the pattern's own length stays fixed at 4
                // beats regardless; only where a resync lands within it
                // changes).
                if (auto sig = pos->getTimeSignature())
                    if (sig->denominator > 0)
                        hostBeatsPerBar = sig->numerator * (4.0 / sig->denominator);
            }
    currentBpm  = bpm;
    bpmFromHost = fromHost;
    drummer.setBpm (bpm);
    drummer.setHostTimeline (hostPlaying, hostHasPpq, hostPpq, hostBeatsPerBar);

    // Play when the user's Play toggle is on, or the host transport is rolling.
    const bool playing = *apvts.getRawParameterValue ("play") > 0.5f || hostPlaying;

    drummer.setStyle (static_cast<DrumPattern::Style> (
        static_cast<int> (*apvts.getRawParameterValue ("style"))));

    // Sound source: 0 = Synth (no samples needed), 1 = Samples.
    drummer.setUseSynth (static_cast<int> (*apvts.getRawParameterValue ("source")) == 0);

    // The 2-D groove axes. Complexity is ALWAYS the grid's — Follow never
    // writes it (see DrumPattern's class comment). When Follow is on and a
    // guide is connected, it modulates intensity — a bounded blend toward the
    // guide's phrase-level "macro" energy around the grid's base value — and
    // drives a velocity-only accent term (DrumPattern::setFollowAccent). With
    // no guide connected, Follow modulates nothing (manual passthrough).
    const float baseComplexity = *apvts.getRawParameterValue ("complexity");
    const float baseIntensity  = *apvts.getRawParameterValue ("intensity");

    float intensityEff = baseIntensity;
    float accent01     = 0.0f;
    float accentDepth  = 0.0f;

    if (follow && guideAvailable)
    {
        // Ceilings keep the grid authoritative even at max Follow strength
        // (0.75 blend ⇒ the grid always keeps >= 25% weight).
        constexpr float kMaxIntensityBlend = 0.75f;
        constexpr float kMaxAccentDepth    = 0.40f;

        const float followAmount = *apvts.getRawParameterValue ("followAmount");
        const float blend        = kMaxIntensityBlend * followAmount;
        intensityEff = baseIntensity + blend * (energyAnalyzer.getMacroEnergy() - baseIntensity);
        accent01     = energyAnalyzer.getAccent();
        accentDepth  = kMaxAccentDepth * followAmount;
    }

    drummer.setComplexity   (baseComplexity);          // always the grid — the hard constraint
    drummer.setIntensity    (intensityEff);
    drummer.setFollowAccent (accent01, accentDepth);
    guideAvailableState.store (follow && guideAvailable, std::memory_order_relaxed);

    currentComplexityState.store (baseComplexity, std::memory_order_relaxed);
    currentIntensityState.store  (intensityEff,   std::memory_order_relaxed);

    // Generate drums into the main output bus. Stopping used to hard-reset
    // the drummer every stopped block, truncating a ringing tail (e.g. the
    // crash decay) with a click. Instead: rewind the playhead once, right on
    // the stop edge, then keep decaying whatever's still active — via
    // renderTails(), which fires nothing new — for as long as the transport
    // stays stopped.
    if (playing)
    {
        drummer.processBlock (mainOut, numSamples);
    }
    else
    {
        if (wasPlaying)
            drummer.rewindPlayhead();
        drummer.renderTails (mainOut, numSamples);
    }
    wasPlaying = playing;

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

            // Re-load the kit this session was saved with — enqueued to the
            // background pool (Step 17), not loaded inline: this callback runs
            // on the host's message thread during session load, and a blocking
            // directory scan + whole-WAV reads (+ resampling since Step 16)
            // here stalls it. The manifest lookup happens inside the job for
            // the same reason (it stats the filesystem); threading the
            // manifest through still matters — without it, a session saved
            // while pointed at a raw, unsorted Salamander download would
            // silently fail to reload, since the remembered root has no
            // per-voice subfolders for the fallback scan to find.
            const auto remembered = apvts.state.getProperty (kSamplesPathId).toString();
            if (remembered.isNotEmpty())
                enqueueKitLoad ({ juce::File (remembered) });
        }
}

// ── Samples ───────────────────────────────────────────────────────────────────

bool AdaptiveDrummerProcessor::loadSamples (const juce::File& samplesRoot)
{
    // Deliberately still synchronous (see the header): the user just picked a
    // folder in the editor and expects an immediate loaded/failed answer, and
    // tests that only need "a kit is loaded" get it deterministically here
    // without pumping a message loop.
    const bool ok = drummer.loadSamples (samplesRoot, findShippedManifest());
    if (ok)
    {
        apvts.state.setProperty (kSamplesPathId, samplesRoot.getFullPathName(), nullptr);

        // The user has explicitly chosen a kit: that counts as "tried", so a
        // later prepareToPlay at an unchanged rate never dispatches an
        // unrelated auto-scan that could race over their choice. A genuine
        // sample-rate change still reloads — through the remembered path just
        // persisted above, i.e. this very kit.
        triedAutoLoad = true;

        // And drop any queued-but-not-started auto-load, so a stale request
        // can't land on top of this explicit load. A job already mid-load can
        // still finish and swap afterwards — that swap is race-free (the C1
        // stress test in DrumSamplerTest.cpp pins it) and its first candidate
        // was the previously remembered path, so the window where it could
        // pick something else is the few milliseconds of an already-running
        // scan.
        const juce::ScopedLock sl (pendingKitLock);
        pendingKitCandidates.clear();
        hasPendingKitLoad = false;
    }
    return ok;
}

// Resolve a kit without forcing the user to re-pick it every session: a folder
// remembered in saved state first, then assets shipped next to the plugin
// binary or the host/standalone executable. The scan-and-load itself runs on
// the background pool (Step 17) — this method only decides and dispatches.
// forceReload (set on an actual sample-rate change) bypasses the "already
// loaded" early-out, since a kit resampled for the old rate must be reloaded
// and re-resampled for the new one, not left as-is; it also bypasses the
// triedAutoLoad latch, since the new rate is worth one fresh scan even when
// the last one found nothing.
void AdaptiveDrummerProcessor::autoLoadSamples (bool forceReload)
{
    if (drummer.areSamplesLoaded() && ! forceReload)
        return;

    // Nothing was found last time and nothing has changed: don't re-dispatch
    // the same doomed scan on every prepareToPlay (pluginval and some hosts
    // cycle prepare/release aggressively).
    if (triedAutoLoad && ! forceReload)
        return;
    triedAutoLoad = true;

    juce::Array<juce::File> candidates;

    // apvts wraps a ValueTree, which is not thread-safe: read the remembered
    // path here, on the calling (message) thread, *before* enqueueing — the
    // pool job never touches apvts (its success write-back is posted back to
    // the message thread; see runKitLoadJob).
    const auto remembered = apvts.state.getProperty (kSamplesPathId).toString();
    if (remembered.isNotEmpty())
        candidates.add (juce::File (remembered));

    for (auto loc : { juce::File::currentExecutableFile,     // the plugin binary
                      juce::File::currentApplicationFile })   // the host / standalone
        candidates.add (juce::File::getSpecialLocation (loc)
                            .getParentDirectory()
                            .getChildFile ("assets/samples/salamander"));

    enqueueKitLoad (std::move (candidates));
}

void AdaptiveDrummerProcessor::enqueueKitLoad (juce::Array<juce::File> candidates)
{
    kitLoadsRequested.fetch_add (1);

    {
        const juce::ScopedLock sl (pendingKitLock);
        pendingKitCandidates = std::move (candidates);
        hasPendingKitLoad    = true;
    }

    bool expected = false;
    if (kitLoadInFlight.compare_exchange_strong (expected, true))
    {
        // The WeakReference is created here, on the message thread: the job
        // needs one for its callAsync write-back, and constructing the *first*
        // WeakReference to an object lazily initialises its shared master
        // state, which must not happen concurrently on the pool thread.
        kitLoadPool.addJob ([this, weakThis = juce::WeakReference<AdaptiveDrummerProcessor> (this)]
                            { runKitLoadJob (weakThis); });
    }
    // else: the running job drains pending requests before clearing
    // kitLoadInFlight (under pendingKitLock — see runKitLoadJob), so the
    // request published above is guaranteed to be consumed; no second job is
    // ever queued while one is in flight.
}

void AdaptiveDrummerProcessor::runKitLoadJob (juce::WeakReference<AdaptiveDrummerProcessor> weakThis)
{
    // Pool thread. drummer.loadSamples() is safe to call from here while the
    // audio thread renders: the sampler builds the new set without any lock
    // held and swaps it in under a SpinLock the audio thread only try-locks
    // (pinned by the C1 stress test in DrumSamplerTest.cpp). apvts is never
    // touched from this thread.
    for (;;)
    {
        juce::Array<juce::File> candidates;
        {
            const juce::ScopedLock sl (pendingKitLock);
            if (! hasPendingKitLoad)
            {
                // Clear the in-flight flag while still holding the lock: an
                // enqueue either published its request before this check (we
                // consume it on the next loop) or runs after the flag is
                // cleared (its CAS dispatches a fresh job). No interleaving
                // leaves a request pending with no job to run it.
                kitLoadInFlight.store (false);
                return;
            }
            candidates.swapWith (pendingKitCandidates);
            hasPendingKitLoad = false;
        }

        const auto manifest = findShippedManifest();
        for (const auto& dir : candidates)
        {
            if (dir.isDirectory() && drummer.loadSamples (dir, manifest))
            {
                // Success-gated persistence, same rule as the old synchronous
                // code: the path is only remembered after loadSamples()
                // confirmed it — a failed load never leaves state pointing at
                // a folder that never actually loaded. Posted to the message
                // thread because apvts.state is a ValueTree (not thread-safe).
                // WeakReference rather than `this`: the host can destroy the
                // processor while this callback is still queued — the
                // destructor joins this *job*, but can't unpost a message
                // already in the queue. Plugin teardown happens on the message
                // thread, so the liveness check can't race the destructor.
                juce::MessageManager::callAsync (
                    [weakThis, path = dir.getFullPathName()]
                    {
                        if (auto* proc = weakThis.get())
                            proc->apvts.state.setProperty (kSamplesPathId, path, nullptr);
                    });
                break;
            }
        }
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
