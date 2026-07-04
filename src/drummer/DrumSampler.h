#pragma once
#include <JuceHeader.h>
#include "DrumPattern.h"

#include <atomic>
#include <unordered_map>

/**
 * DrumSampler — loads WAV samples and renders drum hits into an audio buffer.
 *
 * Two ways to point loadSamples() at a kit:
 *   - A manifest (see assets/salamander_manifest.json) maps each voice to a
 *     list of candidate filenames, matched against one recursive scan of the
 *     root folder — so an unmodified, flat kit download (e.g. Salamander's own
 *     `kick_OH_F_1.wav`-style layout) loads with no manual sorting.
 *   - Per-voice subfolders, one WAV per subfolder is enough:
 *       <root>/kick/   <root>/snare/  <root>/hihat/
 *       <root>/crash/  <root>/ride/   <root>/tom/
 *     Used whenever the manifest is absent, or doesn't resolve a given voice
 *     (per-voice fallback, not all-or-nothing).
 *
 * Compatible with the Salamander Drumkit and similar sample packs.
 * Missing voices produce silence; playback continues for loaded voices.
 *
 * Thread-safety: loadSamples() builds a new sample set on the calling thread
 * (the message thread, or — since Step 17 — the processor's background load
 * pool) and swaps it in under a SpinLock that processBlock() try-locks on the
 * audio thread, so loading never races the audio callback.
 * loadSamples() also resets voices[] (the playback cursors) under the same
 * lock, so processBlock() only needs to hold it around the two things that
 * touch that shared state — claiming a slot (triggerVoice) and reading
 * sample data (mixVoices) — not around computing which voices fire, which
 * reads only this call's own pattern/playhead arguments.
 *
 * Polyphony: each voice has a fixed pool of kSlotsPerVoice playback slots, so
 * up to that many overlapping hits of the same voice — two triggers inside one
 * process block, or a retrigger while the previous hit is still ringing — can
 * play back simultaneously instead of the newer hit overwriting/clicking over
 * an in-flight one. triggerVoice() claims a free slot, or steals the
 * most-advanced one (largest playPos) if the pool is full.
 */
class DrumSampler
{
public:
    DrumSampler();
    ~DrumSampler() = default;

    void prepare (double sampleRate);

    /** manifestFile, if it exists, is tried first for every voice (see the
        class comment); any voice it doesn't resolve falls back to the
        <root>/<voice>/ subfolder scan, unchanged from before manifests existed. */
    bool loadSamples (const juce::File& salamanderRoot, const juce::File& manifestFile = {});
    bool areSamplesLoaded() const noexcept { return anyVoiceLoaded.load (std::memory_order_relaxed); }

    void processBlock (juce::AudioBuffer<float>& outBuffer,
                       int                       numSamples,
                       double                    sampleRate,
                       const DrumPattern&        pattern,
                       double                    bpm,
                       int                       playheadSample,
                       uint32_t                  barIndex);

    /** Decays whatever voices are already active without firing new triggers.
        Call instead of processBlock() while the transport is stopped, so a
        ringing tail finishes naturally rather than being cut off. */
    void renderTail (juce::AudioBuffer<float>& outBuffer, int numSamples);

    void reset();

private:
    static constexpr int kNumVoices     = 6;
    static constexpr int kSlotsPerVoice = 4;
    static const char*   kVoiceDirs[kNumVoices];

    // Immutable-once-built bundle of voice samples. Swapped atomically under
    // sampleLock so the audio thread never reads a half-loaded buffer.
    struct SampleSet
    {
        juce::AudioBuffer<float> samples[kNumVoices];
        bool                     anyLoaded { false };
    };

    // Per-voice-slot playback cursor (audio thread only). Decoupled from sample
    // data so loading a new SampleSet doesn't touch playback state mid-block.
    struct Voice
    {
        int   playPos       { -1 };    // -1 = inactive
        int   triggerOffset { 0 };     // sample offset within the *current* block
        float gain          { 1.0f };  // per-hit velocity (0..1) from the groove table
    };

    bool loadFirstWavInDir (const juce::File& dir, juce::AudioBuffer<float>& dest);
    bool readWavInto       (const juce::File& file, juce::AudioBuffer<float>& dest);
    void triggerVoice      (int voiceIndex, int blockOffset, float velocity01);
    void mixVoices         (juce::AudioBuffer<float>& outBuffer, int numSamples);

    // Recursively scans `root` once for *.wav files, keyed by lowercase filename.
    static std::unordered_map<juce::String, juce::File> buildFlatWavIndex (const juce::File& root);

    // Tries `manifest`'s candidate filenames for `voiceKey` (in priority order)
    // against `flatIndex`; returns the first match, or a default (invalid) File
    // if there's no manifest, no entry for this voice, or none of its
    // candidates were found.
    static juce::File resolveFromManifest (const juce::var& manifest,
                                           const juce::String& voiceKey,
                                           const std::unordered_map<juce::String, juce::File>& flatIndex);

    SampleSet      sampleSet;   // guarded by sampleLock
    juce::SpinLock sampleLock;  // guards sampleSet and voices[] (see the class comment);
                                // held briefly on both sides — the swap in loadSamples(),
                                // and slot-claiming + mixing (not trigger computation) in
                                // processBlock()
    Voice          voices[kNumVoices][kSlotsPerVoice];   // guarded by sampleLock

    juce::AudioFormatManager formatManager;
    std::atomic<double>      currentSampleRate { 44100.0 };   // atomic: prepare() (message thread)
                                                              // can race a background loadSamples()
                                                              // reading it for resampling (Step 17)
    std::atomic<bool>        anyVoiceLoaded     { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumSampler)
};
