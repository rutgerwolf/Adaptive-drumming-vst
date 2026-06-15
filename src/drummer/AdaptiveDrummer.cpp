#include "AdaptiveDrummer.h"

#include <cmath>

AdaptiveDrummer::AdaptiveDrummer()
{
    drumPattern.loadStyle (DrumPattern::Style::Rock);
}

void AdaptiveDrummer::prepare (double sampleRate, int /*blockSize*/)
{
    currentSampleRate = sampleRate;
    drumSampler.prepare (sampleRate);
    reset();
}

void AdaptiveDrummer::reset()
{
    playheadSample = 0;
    drumSampler.reset();
}

void AdaptiveDrummer::setStyle (DrumPattern::Style style)
{
    drumPattern.loadStyle (style);
}

void AdaptiveDrummer::setBpm (double newBpm)
{
    bpm = juce::jlimit (40.0, 240.0, newBpm);
}

void AdaptiveDrummer::setDensity (DrumPattern::Density density)
{
    drumPattern.setDensity (density);
}

void AdaptiveDrummer::setHostTimeline (bool isPlaying, double ppqPosition) noexcept
{
    hostIsPlaying   = isPlaying;
    hostPpqPosition = ppqPosition;
}

bool AdaptiveDrummer::loadSamples (const juce::File& root)
{
    return drumSampler.loadSamples (root);
}

int AdaptiveDrummer::ppqToPlayhead (double ppqPosition, int patternLenSamples,
                                    double beatsPerBar) noexcept
{
    if (patternLenSamples <= 0 || beatsPerBar <= 0.0)
        return 0;

    // Fraction of the way through the current bar, wrapped to [0, 1).
    double bars = ppqPosition / beatsPerBar;
    double frac = bars - std::floor (bars);

    int s = static_cast<int> (std::llround (frac * patternLenSamples));
    return ((s % patternLenSamples) + patternLenSamples) % patternLenSamples;
}

void AdaptiveDrummer::processBlock (juce::AudioBuffer<float>& outBuffer, int numSamples)
{
    const int patternLen = drumPattern.getLengthInSamples (bpm, currentSampleRate);

    // B3: when the host transport is running, lock the in-pattern position to
    // the DAW timeline; otherwise keep free-running from the internal counter.
    if (hostIsPlaying && patternLen > 0)
        playheadSample = ppqToPlayhead (hostPpqPosition, patternLen);

    drumSampler.processBlock (outBuffer, numSamples, currentSampleRate,
                              drumPattern, bpm, playheadSample);

    // Advance the free-running counter (used when the host isn't playing; when
    // it is, the next block overwrites this from ppq anyway).
    playheadSample = (playheadSample + numSamples) % juce::jmax (1, patternLen);
}
