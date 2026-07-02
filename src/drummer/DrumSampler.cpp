#include "DrumSampler.h"
#include "DrumStepClock.h"

#include <utility>

const char* DrumSampler::kVoiceDirs[DrumSampler::kNumVoices] =
    { "kick", "snare", "hihat", "crash", "ride", "tom" };

DrumSampler::DrumSampler()
{
    formatManager.registerBasicFormats();
}

void DrumSampler::prepare (double sampleRate)
{
    currentSampleRate = sampleRate;
}

bool DrumSampler::loadSamples (const juce::File& root)
{
    if (! root.isDirectory())
    {
        DBG ("DrumSampler: root not found: " + root.getFullPathName());
        anyVoiceLoaded.store (false, std::memory_order_relaxed);
        return false;
    }

    // Build the new set off the audio thread — no lock held during file I/O.
    SampleSet newSet;
    for (int i = 0; i < kNumVoices; ++i)
    {
        const auto dir = root.getChildFile (kVoiceDirs[i]);
        if (loadFirstWavInDir (dir, newSet.samples[i]))
        {
            newSet.anyLoaded = true;
            DBG ("DrumSampler: loaded " + juce::String (kVoiceDirs[i]));
        }
        else
        {
            DBG ("DrumSampler: no WAV for " + juce::String (kVoiceDirs[i]) + " — silent");
        }
    }

    const bool loaded = newSet.anyLoaded;

    // Swap the new set in under the lock the audio thread try-locks. The swap is
    // O(1) (AudioBuffer moves), so the audio thread almost never waits. After the
    // swap `newSet` owns the *old* buffers, which are freed here on this thread.
    {
        const juce::SpinLock::ScopedLockType sl (sampleLock);
        std::swap (sampleSet, newSet);
        for (auto& perVoice : voices)
            for (auto& v : perVoice)
            {
                v.playPos       = -1;
                v.triggerOffset = 0;
                v.gain          = 1.0f;
            }
    }

    anyVoiceLoaded.store (loaded, std::memory_order_relaxed);
    return loaded;
}

void DrumSampler::processBlock (juce::AudioBuffer<float>& outBuffer,
                                int                       numSamples,
                                double                    sampleRate,
                                const DrumPattern&        pattern,
                                double                    bpm,
                                int                       playheadSample)
{
    // If loadSamples() is mid-swap, skip this block rather than race the buffers.
    const juce::SpinLock::ScopedTryLockType sl (sampleLock);
    if (! sl.isLocked()) return;

    forEachStepTrigger (pattern, playheadSample, numSamples, bpm, sampleRate,
                        [this] (int voice, int offset) { triggerVoice (voice, offset); });

    mixVoices (outBuffer, numSamples);
}

void DrumSampler::reset()
{
    for (auto& perVoice : voices)
        for (auto& v : perVoice)
        {
            v.playPos       = -1;
            v.triggerOffset = 0;
            v.gain          = 1.0f;
        }
}

// ── private ──────────────────────────────────────────────────────────────────

bool DrumSampler::loadFirstWavInDir (const juce::File& dir,
                                     juce::AudioBuffer<float>& dest)
{
    if (! dir.isDirectory()) return false;

    const char* preferredNames[] = {
        "kick.wav", "snare.wav", "hihat-closed.wav", "hihat.wav",
        "crash.wav", "ride.wav", "tom-mid.wav", "tom.wav"
    };

    juce::File target;
    for (const char* name : preferredNames)
    {
        const auto f = dir.getChildFile (name);
        if (f.existsAsFile()) { target = f; break; }
    }

    if (! target.existsAsFile())
    {
        for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*.wav"))
        {
            target = entry.getFile();
            break;
        }
    }

    if (! target.existsAsFile()) return false;

    std::unique_ptr<juce::AudioFormatReader> reader (
        formatManager.createReaderFor (target));
    if (reader == nullptr) return false;

    dest.setSize (static_cast<int> (reader->numChannels),
                  static_cast<int> (reader->lengthInSamples));
    reader->read (&dest, 0, static_cast<int> (reader->lengthInSamples), 0, true, true);
    return dest.getNumSamples() > 0;
}

void DrumSampler::triggerVoice (int voiceIndex, int blockOffset)
{
    auto* slots = voices[voiceIndex];

    // Prefer a free slot.
    int chosen = -1;
    for (int s = 0; s < kSlotsPerVoice; ++s)
    {
        if (slots[s].playPos < 0)
        {
            chosen = s;
            break;
        }
    }

    // Pool full: steal the most-advanced slot (largest playPos) — it is
    // closest to finishing, so cutting it off is the least noticeable.
    if (chosen < 0)
    {
        chosen = 0;
        for (int s = 1; s < kSlotsPerVoice; ++s)
            if (slots[s].playPos > slots[chosen].playPos)
                chosen = s;
    }

    auto& v = slots[chosen];
    v.playPos       = 0;
    v.triggerOffset = blockOffset;
    v.gain          = 1.0f;
}

void DrumSampler::mixVoices (juce::AudioBuffer<float>& outBuffer, int numSamples)
{
    for (int i = 0; i < kNumVoices; ++i)
    {
        const auto& sample = sampleSet.samples[i];
        if (sample.getNumSamples() == 0) continue;

        for (auto& v : voices[i])
        {
            if (v.playPos < 0) continue;

            // A newly-loaded set may be shorter than where an in-flight cursor sits.
            if (v.playPos >= sample.getNumSamples()) { v.playPos = -1; continue; }

            const int startInBlock = v.triggerOffset;
            const int blockRemain  = numSamples - startInBlock;
            if (blockRemain <= 0) continue;

            const int sampleRemain = sample.getNumSamples() - v.playPos;
            const int toCopy       = juce::jmin (blockRemain, sampleRemain);

            for (int ch = 0; ch < outBuffer.getNumChannels(); ++ch)
            {
                const int srcCh = juce::jmin (ch, sample.getNumChannels() - 1);
                outBuffer.addFrom (ch, startInBlock,
                                   sample, srcCh, v.playPos, toCopy, v.gain);
            }

            v.playPos += toCopy;
            if (v.playPos >= sample.getNumSamples())
                v.playPos = -1;

            // B1 fix: once this block has been served, the note continues from the
            // start of the next block. Without this reset, every subsequent block
            // re-skips `triggerOffset` samples → a recurring gap/click on any sample
            // longer than one buffer. Applies per slot so each overlapping hit
            // tracks its own mid-block start correctly.
            v.triggerOffset = 0;
        }
    }
}
