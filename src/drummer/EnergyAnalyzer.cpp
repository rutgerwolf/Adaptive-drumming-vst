#include "EnergyAnalyzer.h"

#include <cmath>

void EnergyAnalyzer::prepare (double newSampleRate, int /*blockSize*/)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    reset();
}

void EnergyAnalyzer::reset()
{
    envelopeLevel = 0.0f;
    macroLevel    = 0.0f;
    energy.store      (0.0f, std::memory_order_relaxed);
    macroEnergy.store (0.0f, std::memory_order_relaxed);
    accent.store      (0.0f, std::memory_order_relaxed);
}

void EnergyAnalyzer::processBlock (const juce::AudioBuffer<float>& guide, int numSamples)
{
    const int numCh = guide.getNumChannels();
    if (numCh <= 0 || numSamples <= 0) { processSilence (numSamples); return; }

    // Mean square across all channels → RMS.
    double sumSquares = 0.0;
    for (int ch = 0; ch < numCh; ++ch)
    {
        const float* d = guide.getReadPointer (ch);
        for (int i = 0; i < numSamples; ++i)
            sumSquares += static_cast<double> (d[i]) * d[i];
    }

    const double meanSquare = sumSquares / (static_cast<double> (numSamples) * numCh);
    const float  rms        = static_cast<float> (std::sqrt (meanSquare));

    // RMS → dB → normalised 0..1 over [kMinDb, kMaxDb].
    const float db     = juce::Decibels::gainToDecibels (rms, -100.0f);
    const float target = juce::jlimit (0.0f, 1.0f, (db - kMinDb) / (kMaxDb - kMinDb));

    updateEnvelope (target, numSamples);
}

void EnergyAnalyzer::processSilence (int numSamples)
{
    updateEnvelope (0.0f, juce::jmax (1, numSamples));
}

void EnergyAnalyzer::updateEnvelope (float targetEnergy, int numSamples)
{
    // Time-constant smoothing that is independent of the block size.
    const double blockSec = static_cast<double> (numSamples) / sampleRate;
    const float  tc       = (targetEnergy > envelopeLevel) ? attackTimeSec : releaseTimeSec;
    const float  coeff    = static_cast<float> (std::exp (-blockSec / juce::jmax (1.0e-4f, tc)));

    envelopeLevel = targetEnergy + coeff * (envelopeLevel - targetEnergy);

    const float e = juce::jlimit (0.0f, 1.0f, envelopeLevel);
    energy.store (e, std::memory_order_relaxed);

    // A slower "macro" envelope of the same target — the phrase-level contour
    // Follow's intensity modulation tracks (PluginProcessor::processBlock).
    const float macroTc    = (targetEnergy > macroLevel) ? kMacroAttackSec : kMacroReleaseSec;
    const float macroCoeff = static_cast<float> (std::exp (-blockSec / juce::jmax (1.0e-4f, macroTc)));
    macroLevel = targetEnergy + macroCoeff * (macroLevel - targetEnergy);

    const float m = juce::jlimit (0.0f, 1.0f, macroLevel);
    macroEnergy.store (m, std::memory_order_relaxed);

    // Accent: how far the fast envelope currently pokes above the slower
    // macro contour, past a margin that rejects steady-signal jitter.
    // Self-normalising — "louder than the guide's own recent level", not an
    // absolute threshold, so a hot-mixed guide doesn't read as constant accents.
    accent.store (juce::jlimit (0.0f, 1.0f, (e - m - kAccentMargin) * kAccentGain),
                 std::memory_order_relaxed);
}
