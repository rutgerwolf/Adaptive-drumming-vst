#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cstring>
#include <type_traits>

// Where the last successfully-loaded sample folder is remembered in plugin state.
static const juce::Identifier kSamplesPathId { "samplesPath" };

// Where the current groove seed (see AdaptiveDrummer::setSeed) is remembered
// in plugin state — a plain property, not an APVTS parameter (see the header
// comment on randomizeGrooveSeed()).
static const juce::Identifier kGrooveSeedId { "grooveSeed" };

// Where the per-voice mute bitmask (bit 0 Kick … bit 5 Tom; set = muted) is
// remembered in plugin state — same plain-property pattern as the groove
// seed. Absent (pre-mute sessions) means 0: nothing muted.
static const juce::Identifier kMuteMaskId { "muteMask" };

// Where the user-drawn custom grid is remembered in plugin state: the raw
// GrooveTable bytes, base64-encoded (juce::Base64) into a plain string
// property, guarded by an explicit format-version int so a future layout
// change can detect and reject/migrate old blobs instead of misinterpreting
// bytes. Absence of gridData (or a gridVersion mismatch, or a blob of the
// wrong size) means "no custom grid" — normal for pre-grid or foreign
// sessions, handled silently. The blob is raw little-endian struct bytes
// (including padding); all supported targets are little-endian, and the
// version property is the escape hatch if that ever changes.
static const juce::Identifier kGridDataId    { "gridData" };
static const juce::Identifier kGridVersionId { "gridVersion" };
static constexpr int kGridFormatVersion = 1;

// The raw-bytes serialization above is only sound while GrooveTable stays a
// tightly-packed trivially-copyable POD of the exact size version 1 was
// authored against. Any change here is a gridVersion bump, not a silent edit.
static_assert (std::is_trivially_copyable<GrooveTable>::value,
               "gridData persists GrooveTable as raw bytes");
static_assert (sizeof (GrooveTable) == 584,
               "GrooveTable layout changed — bump kGridFormatVersion and migrate gridData");

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
          // Enabled by default: some hosts' sidechain-routing UI (observed in
          // Adobe Audition) only lets a user route into a bus the plugin
          // already reports as active -- it never activates a disabled one on
          // the user's behalf. A disabled-by-default Sidechain silently never
          // received audio in that case (Follow would read as connected in
          // our own code once a host routes into it, but nothing arrived).
          // Enabled-by-default costs two always-present input channels in
          // hosts that never touch Follow, but without it Follow -- the
          // plugin's actual reason to exist over a plain drum machine -- is
          // unusable in at least one real host.
          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "AdaptiveDrummer", createParameterLayout())
{
    // Baseline for the producer poll's style-change edge detector (decision:
    // a style pick exits custom-grid mode) — must start at the parameter's
    // actual value so the first poll can't see a phantom "change".
    lastSeenStyleIdx = static_cast<int> (*apvts.getRawParameterValue ("style"));

    // The dynamic-table producer's message-thread poll — see timerCallback()
    // in the header for why a processor-side timer is the right mechanism.
    startTimerHz (20);
}

AdaptiveDrummerProcessor::~AdaptiveDrummerProcessor()
{
    stopTimer();

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

    drummer.setSeed (currentGrooveSeed.load (std::memory_order_relaxed));

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
    apvts.state.setProperty (kGrooveSeedId,
                             static_cast<int> (currentGrooveSeed.load (std::memory_order_relaxed)),
                             nullptr);
    apvts.state.setProperty (kMuteMaskId,
                             static_cast<int> (muteMask.load (std::memory_order_relaxed)),
                             nullptr);
    persistGridToState();   // kept current on every edit too — this is belt-and-braces

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

            // A session saved before the seed feature existed has no
            // grooveSeed property; keep the built-in default in that case.
            if (apvts.state.hasProperty (kGrooveSeedId))
                currentGrooveSeed.store (
                    static_cast<uint32_t> (static_cast<int> (apvts.state.getProperty (kGrooveSeedId))),
                    std::memory_order_relaxed);

            // Per-voice mutes. Unlike the seed, an absent property must
            // actively reset to 0 (nothing muted): loading a pre-mute session
            // into an instance that currently has voices muted should clear
            // them, not keep them. Restored mutes are (re)published below —
            // this callback runs on the host's message thread (see the kit
            // note above) — rather than waiting a timer tick.
            muteMask.store (
                static_cast<uint8_t> (static_cast<int> (apvts.state.getProperty (kMuteMaskId, 0)) & 0x3F),
                std::memory_order_relaxed);

            // The restored session's style is the new edge-detect baseline —
            // a loaded style must never read as "the user picked a style"
            // (which would wrongly discard the custom grid restored below).
            lastSeenStyleIdx = static_cast<int> (*apvts.getRawParameterValue ("style"));

            // Custom grid. Like the mute mask, absence actively resets (a
            // pre-grid session loaded into an instance with a drawing must
            // clear it). A valid restored grid is treated exactly like a
            // freshly-drawn one: fed through the same producer below, so
            // mutes still compose correctly on top.
            customGridActive = false;
            if (static_cast<int> (apvts.state.getProperty (kGridVersionId, 0)) == kGridFormatVersion)
            {
                juce::MemoryOutputStream decoded;
                if (juce::Base64::convertFromBase64 (decoded,
                        apvts.state.getProperty (kGridDataId).toString())
                    && decoded.getDataSize() == sizeof (GrooveTable))
                {
                    std::memcpy (&customGrid, decoded.getData(), sizeof (GrooveTable));
                    customGridActive = true;
                }
            }
            // Bump unconditionally: "custom grid A" → "custom grid B" leaves
            // every other publish input identical, and the revision is what
            // makes the producer see that as a change.
            ++gridRevision;

            publishDynamicTableIfNeeded();
        }
}

// ── Groove seed ───────────────────────────────────────────────────────────────

void AdaptiveDrummerProcessor::randomizeGrooveSeed()
{
    const auto newSeed = static_cast<uint32_t> (juce::Random::getSystemRandom().nextInt64());
    currentGrooveSeed.store (newSeed, std::memory_order_relaxed);
    apvts.state.setProperty (kGrooveSeedId, static_cast<int> (newSeed), nullptr);
    drummer.setSeed (newSeed);
}

// ── Per-voice mutes ───────────────────────────────────────────────────────────

void AdaptiveDrummerProcessor::setVoiceMuted (int voice, bool shouldMute)
{
    if (voice < 0 || voice >= GrooveTable::kNumVoices)
        return;

    const auto bit = static_cast<uint8_t> (1u << voice);
    auto mask = muteMask.load (std::memory_order_relaxed);
    mask = shouldMute ? static_cast<uint8_t> (mask | bit)
                      : static_cast<uint8_t> (mask & ~bit);
    muteMask.store (mask, std::memory_order_relaxed);

    apvts.state.setProperty (kMuteMaskId, static_cast<int> (mask), nullptr);

    // Message thread already — publish synchronously rather than waiting up
    // to one timer tick, so the click lands at the very next bar wrap.
    publishDynamicTableIfNeeded();
}

// ── User-drawable groove grid ─────────────────────────────────────────────────

const GrooveTable& AdaptiveDrummerProcessor::getGridTable() const noexcept
{
    if (customGridActive)
        return customGrid;

    // The TARGET style (the raw parameter), not drummer.getStyle(): the grid
    // must show the style the user just picked, not wait out the bar latch.
    return DrumPattern::getStyleTable (static_cast<DrumPattern::Style> (
        static_cast<int> (*apvts.getRawParameterValue ("style"))));
}

void AdaptiveDrummerProcessor::setGridCell (int voice, int step, bool on, int velocity127)
{
    if (voice < 0 || voice >= GrooveTable::kNumVoices
        || step < 0 || step  >= GrooveTable::kNumSteps)
        return;

    if (! customGridActive)
    {
        // First edit enters custom mode: start from the target style's
        // built-in table (with mutes composing on top at publish time, this
        // is exactly what was audible the instant before the edit). swing and
        // styleSalt come along with the copy, so drawing on Jazz keeps its
        // swing and its remaining probabilistic cells keep their per-bar
        // variation.
        customGrid       = getGridTable();
        customGridActive = true;
    }

    if (on)
    {
        // Hand-drawn hit: always fires (probability 255, threshold 0 — the
        // "deterministic backbone" convention), at exactly the drawn velocity
        // (velLow == velHigh: no intensity-axis re-voicing; velRandRange 0:
        // no humanization jitter on a hand-placed hit), no accent/ghost flags.
        const auto vel = static_cast<uint8_t> (juce::jlimit (1, 127, velocity127));
        customGrid.cells[voice][step] = StepCell { vel, vel, 0, 255, 0, 0 };
    }
    else
    {
        // Off = never plays: velHigh == 0 via full zeroing, the exact
        // convention the mute producer already uses.
        customGrid.cells[voice][step] = StepCell {};
    }

    ++gridRevision;
    persistGridToState();
    publishDynamicTableIfNeeded();   // message thread — lands at the next bar wrap
}

void AdaptiveDrummerProcessor::clearCustomGrid()
{
    if (! customGridActive)
        return;

    customGridActive = false;
    persistGridToState();

    // Must publish (not just flip the flag): the drummer may be playing an
    // adopted custom-grid slot, and only a fresh publish of plain
    // style + mutes can supersede it — releaseActive() is consumer-side and
    // unreachable from here.
    publishDynamicTableIfNeeded();
}

void AdaptiveDrummerProcessor::persistGridToState()
{
    if (customGridActive)
    {
        apvts.state.setProperty (kGridVersionId, kGridFormatVersion, nullptr);
        apvts.state.setProperty (kGridDataId,
                                 juce::Base64::toBase64 (&customGrid, sizeof (GrooveTable)),
                                 nullptr);
    }
    else
    {
        apvts.state.removeProperty (kGridDataId,    nullptr);
        apvts.state.removeProperty (kGridVersionId, nullptr);
    }
}

void AdaptiveDrummerProcessor::timerCallback()
{
    publishDynamicTableIfNeeded();
}

// Why this is safe against the style-flip race (the reasoning a reviewer will
// ask for):
//
// Style switches are bar-latched: processBlock() records the "style" parameter
// into the drummer every block, but the actual flip happens inside
// AdaptiveDrummer::processBlock() at the next step-0 bar wrap — and at that
// exact moment applyPendingStyle() drops any adopted dynamic table
// (tableExchange.releaseActive()), falling back to the new style's *unmuted*
// built-in table. So on a style change this producer must get a fresh copy of
// the NEW intended table (new style + mutes; the custom grid is discarded by
// a style pick — see below) into the mailbox before (or by) that wrap.
//
// Two properties make that work:
//
//   1. This routine keys off the "style" PARAMETER (the target style, visible
//      the instant it changes — from the editor or host automation alike),
//      never drummer.getStyle() (bar-latched: it only reports the change
//      after the very wrap we need to beat). The 20 Hz poll therefore reacts
//      within ≤50 ms of any style change, while the flip it must beat is at
//      least one bar away — ≥1 s at the 240 BPM clamp in setBpm(). (Editor
//      mute clicks and grid edits don't even wait for the poll: their setters
//      call this synchronously.)
//   2. The consumer's wrap sequence is applyPendingStyle() *then* consume()
//      (see AdaptiveDrummer::processBlock). So even when the rebuilt table
//      for the new style is published before the flip's own wrap, the same
//      wrap that loads the new built-in table and releases the old dynamic
//      slot immediately adopts the waiting copy — the documented same-wrap
//      tie-break ("the dynamic table wins because consume() runs after
//      applyPendingStyle()"). No wrong-sounding bar, no audible gap.
//
// A style pick exits custom-grid mode (roadmap decision A): picking a style
// means choosing that built-in groove — exactly the semantics
// applyPendingStyle() already implements by dropping the dynamic table — so
// the edge detector below discards the drawing the moment the parameter
// changes. Host automation of "style" gets the same treatment as a button
// click; there is no way (and no reason) to distinguish them here.
//
// Why a style change publishes even with mask == 0 and no custom grid (a
// deliberate strengthening of the original mute-only producer, which skipped
// that case): once anything has ever been published, the mailbox may hold a
// stale unconsumed table — e.g. unmute publishes a clean copy of the OLD
// style, then the style changes within the same bar. At the wrap,
// applyPendingStyle() loads the new style but consume() runs right after it
// and would adopt that stale old-style copy, which then plays indefinitely
// (nothing else would trigger a republish). The message thread cannot know
// whether a pending publish has been consumed, so after the first publish it
// always supersedes on any relevant change; a copy that happens to be
// byte-identical to the new built-in table is harmless. everPublished keeps
// the never-touched case (no mute, no grid, ever) provably inert.
//
// Residual, accepted transient: if the inputs change again while both mailbox
// slots are held (one active + one pending), tryStage() returns nullptr and
// this update coalesces into a later tick — per the mailbox contract that
// state resolves within one bar (every wrap consume()s), so a stale table can
// play for at most one bar before the retry lands. Last-writer-wins
// publishing means only the newest (style, mask, grid) ever matters.
void AdaptiveDrummerProcessor::publishDynamicTableIfNeeded()
{
    // Message thread only — this is TableExchange's single producer.
    const auto mask     = muteMask.load (std::memory_order_relaxed);
    const int  styleIdx = static_cast<int> (*apvts.getRawParameterValue ("style"));

    // Decision A's edge detector: a style pick discards the custom grid.
    if (styleIdx != lastSeenStyleIdx)
    {
        lastSeenStyleIdx = styleIdx;
        if (customGridActive)
        {
            customGridActive = false;
            persistGridToState();
        }
    }

    // Idempotence. Before anything has ever been published the mailbox stays
    // untouched unless a mute or a custom grid actually needs it; afterwards,
    // any change to the publish inputs supersedes (see the comment above for
    // why that includes style-only changes with nothing muted/drawn).
    if (everPublished)
    {
        if (mask == lastPublishedMuteMask
            && styleIdx == lastPublishedStyle
            && customGridActive == lastPublishedCustom
            && (! customGridActive || gridRevision == lastPublishedGridRev))
            return;
    }
    else if (mask == 0 && ! customGridActive)
        return;

    auto& exchange = drummer.getTableExchange();
    GrooveTable* slot = exchange.tryStage();
    if (slot == nullptr)
        return;   // both slots held — resolves within one bar; the next timer tick retries

    // The intended table: the custom grid when active, else the target
    // style's built-in table — then mutes compose on top by zeroing the whole
    // StepCell for every step of a muted voice's row. velHigh == 0 is the
    // model's authored "this cell never plays" convention — stepFires()
    // rejects such a cell before any threshold/probability logic runs (see
    // DrumPattern.cpp), so this guarantees silence at every complexity/
    // intensity/seed. Composing here (rather than baking mutes into
    // customGrid) keeps muting non-destructive to the drawing.
    *slot = customGridActive
                ? customGrid
                : DrumPattern::getStyleTable (static_cast<DrumPattern::Style> (styleIdx));
    for (int v = 0; v < GrooveTable::kNumVoices; ++v)
        if ((mask & (1u << v)) != 0)
            for (int s = 0; s < GrooveTable::kNumSteps; ++s)
                slot->cells[v][s] = StepCell {};

    exchange.publish();
    everPublished         = true;
    lastPublishedMuteMask = mask;
    lastPublishedStyle    = styleIdx;
    lastPublishedCustom   = customGridActive;
    lastPublishedGridRev  = gridRevision;
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
