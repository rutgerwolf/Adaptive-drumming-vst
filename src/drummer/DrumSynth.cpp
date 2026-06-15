#include "DrumSynth.h"
#include "DrumStepClock.h"

#include <cmath>

void DrumSynth::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    reset();
}

void DrumSynth::reset()
{
    for (auto& v : voices)
    {
        v.active      = false;
        v.t           = 0.0;
        v.phase       = 0.0;
        v.startOffset = 0;
        v.noisePrev   = 0.0f;
    }
}

void DrumSynth::triggerVoice (int voiceIndex, int blockOffset)
{
    auto& v = voices[voiceIndex];
    v.active      = true;
    v.t           = 0.0;
    v.phase       = 0.0;
    v.startOffset = blockOffset;
    v.noisePrev   = 0.0f;
}

void DrumSynth::processBlock (juce::AudioBuffer<float>& outBuffer,
                              int                       numSamples,
                              double                    newSampleRate,
                              const DrumPattern&        pattern,
                              double                    bpm,
                              int                       playheadSample)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : sampleRate;

    forEachStepTrigger (pattern, playheadSample, numSamples, bpm, sampleRate,
                        [this] (int voice, int offset) { triggerVoice (voice, offset); });

    const int numCh = juce::jmin (outBuffer.getNumChannels(), 8);
    float*    chans[8] { nullptr };
    for (int ch = 0; ch < numCh; ++ch)
        chans[ch] = outBuffer.getWritePointer (ch);

    for (int i = 0; i < numSamples; ++i)
    {
        float s = 0.0f;
        for (int vIdx = 0; vIdx < kNumVoices; ++vIdx)
        {
            auto& v = voices[vIdx];
            if (! v.active || i < v.startOffset)
                continue;
            s += renderVoice (vIdx, v);
        }

        if (s != 0.0f)
            for (int ch = 0; ch < numCh; ++ch)
                chans[ch][i] += s;
    }

    // A note started this block at startOffset; from the next block it continues
    // from the start of the buffer.
    for (auto& v : voices)
        v.startOffset = 0;
}

float DrumSynth::renderVoice (int voiceIndex, Voice& v)
{
    const double t          = v.t;
    const double twoPiOverSr = juce::MathConstants<double>::twoPi / sampleRate;

    // One-pole-ish highpass on white noise (a cheap differentiator) for the
    // cymbal/hat voices.
    auto noiseHP = [&v, this]
    {
        const float raw = rng.nextFloat() * 2.0f - 1.0f;
        const float hp  = raw - v.noisePrev;
        v.noisePrev = raw;
        return hp;
    };

    float out  = 0.0f;
    bool  done = false;

    switch (voiceIndex)
    {
        case 0:  // Kick — pitch-swept sine
        {
            const double f = 48.0 + 90.0 * std::exp (-t / 0.035);
            v.phase += f * twoPiOverSr;
            out  = std::sin ((float) v.phase) * (float) std::exp (-t / 0.11) * 0.9f;
            done = t > 0.40;
            break;
        }
        case 1:  // Snare — tonal body + noise
        {
            const float body  = std::sin ((float) (juce::MathConstants<double>::twoPi * 185.0 * t))
                                 * (float) std::exp (-t / 0.09) * 0.5f;
            const float noise = noiseHP() * (float) std::exp (-t / 0.06) * 0.6f;
            out  = body + noise;
            done = t > 0.22;
            break;
        }
        case 2:  // HiHat (closed) — short bright noise
        {
            out  = noiseHP() * (float) std::exp (-t / 0.013) * 0.5f;
            done = t > 0.06;
            break;
        }
        case 3:  // Crash — long noise decay
        {
            out  = noiseHP() * (float) std::exp (-t / 0.5) * 0.4f;
            done = t > 1.2;
            break;
        }
        case 4:  // Ride — noise wash + tonal ping
        {
            const float ping = std::sin ((float) (juce::MathConstants<double>::twoPi * 520.0 * t))
                               * (float) std::exp (-t / 0.2) * 0.18f;
            out  = noiseHP() * (float) std::exp (-t / 0.28) * 0.25f + ping;
            done = t > 0.6;
            break;
        }
        default: // 5 Tom — pitch-swept sine (lower)
        {
            const double f = 90.0 + 120.0 * std::exp (-t / 0.22);
            v.phase += f * twoPiOverSr;
            out  = std::sin ((float) v.phase) * (float) std::exp (-t / 0.20) * 0.7f;
            done = t > 0.45;
            break;
        }
    }

    v.t += 1.0 / sampleRate;
    if (done)
        v.active = false;

    return out;
}
