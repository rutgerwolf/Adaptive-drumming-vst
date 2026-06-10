#include "DrumSampler.h"

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
    anyVoiceLoaded = false;

    if (! root.isDirectory())
    {
        DBG ("DrumSampler: root not found: " + root.getFullPathName());
        return false;
    }

    for (int i = 0; i < kNumVoices; ++i)
    {
        const auto dir = root.getChildFile (kVoiceDirs[i]);
        if (loadFirstWavInDir (dir, voices[i].sample))
        {
            anyVoiceLoaded = true;
            DBG ("DrumSampler: loaded " + juce::String (kVoiceDirs[i]));
        }
        else
        {
            DBG ("DrumSampler: no WAV for " + juce::String (kVoiceDirs[i]) + " — silent");
        }
    }

    return anyVoiceLoaded;
}

void DrumSampler::processBlock (juce::AudioBuffer<float>& outBuffer,
                                int                       numSamples,
                                double                    sampleRate,
                                const DrumPattern&        pattern,
                                double                    bpm,
                                int                       playheadSample)
{
    const int patternLen = pattern.getLengthInSamples (bpm, sampleRate);
    if (patternLen <= 0) return;

    const int stepLen = patternLen / DrumPattern::kSteps;
    if (stepLen <= 0) return;

    for (int offset = 0; offset < numSamples; ++offset)
    {
        const int posInPattern = (playheadSample + offset) % patternLen;

        if (posInPattern % stepLen == 0)
        {
            const int     step = posInPattern / stepLen;
            const uint8_t mask = pattern.getStep (step);

            if (mask & DrumPattern::Kick)  triggerVoice (0, offset);
            if (mask & DrumPattern::Snare) triggerVoice (1, offset);
            if (mask & DrumPattern::HiHat) triggerVoice (2, offset);
            if (mask & DrumPattern::Crash) triggerVoice (3, offset);
            if (mask & DrumPattern::Ride)  triggerVoice (4, offset);
            if (mask & DrumPattern::Tom)   triggerVoice (5, offset);
        }
    }

    mixVoices (outBuffer, numSamples);
}

void DrumSampler::reset()
{
    for (auto& v : voices)
    {
        v.playPos       = -1;
        v.triggerOffset = 0;
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
    voices[voiceIndex].playPos       = 0;
    voices[voiceIndex].triggerOffset = blockOffset;
}

void DrumSampler::mixVoices (juce::AudioBuffer<float>& outBuffer, int numSamples)
{
    for (auto& v : voices)
    {
        if (v.playPos < 0 || v.sample.getNumSamples() == 0) continue;

        const int startInBlock = v.triggerOffset;
        const int blockRemain  = numSamples - startInBlock;
        if (blockRemain <= 0) continue;

        const int sampleRemain = v.sample.getNumSamples() - v.playPos;
        const int toCopy       = juce::jmin (blockRemain, sampleRemain);

        for (int ch = 0; ch < outBuffer.getNumChannels(); ++ch)
        {
            const int srcCh = juce::jmin (ch, v.sample.getNumChannels() - 1);
            outBuffer.addFrom (ch, startInBlock,
                               v.sample, srcCh, v.playPos, toCopy);
        }

        v.playPos += toCopy;
        if (v.playPos >= v.sample.getNumSamples())
            v.playPos = -1;
    }
}
