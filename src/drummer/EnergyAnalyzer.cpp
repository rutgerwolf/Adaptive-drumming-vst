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
    energy.store (0.0f, std::memory_order_relaxed);
    densityState.store (static_cast<int> (DrumPattern::Density::Sparse),
                        std::memory_order_relaxed);
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

    const auto next = nextDensity (getDensity(), e);
    densityState.store (static_cast<int> (next), std::memory_order_relaxed);
}

DrumPattern::Density EnergyAnalyzer::nextDensity (DrumPattern::Density current, float e) noexcept
{
    switch (current)
    {
        case DrumPattern::Density::Sparse:
            if (e > kUpToFull)    return DrumPattern::Density::Full;   // big jump in one step
            if (e > kUpToMedium)  return DrumPattern::Density::Medium;
            return DrumPattern::Density::Sparse;

        case DrumPattern::Density::Medium:
            if (e > kUpToFull)    return DrumPattern::Density::Full;
            if (e < kDownToSparse) return DrumPattern::Density::Sparse;
            return DrumPattern::Density::Medium;

        case DrumPattern::Density::Full:
        default:
            if (e < kDownToSparse) return DrumPattern::Density::Sparse;
            if (e < kDownToMedium) return DrumPattern::Density::Medium;
            return DrumPattern::Density::Full;
    }
}
