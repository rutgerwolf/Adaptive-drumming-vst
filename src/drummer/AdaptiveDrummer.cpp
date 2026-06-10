#include "AdaptiveDrummer.h"

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

bool AdaptiveDrummer::loadSamples (const juce::File& root)
{
    return drumSampler.loadSamples (root);
}

void AdaptiveDrummer::processBlock (juce::AudioBuffer<float>& outBuffer, int numSamples)
{
    drumSampler.processBlock (outBuffer, numSamples, currentSampleRate,
                              drumPattern, bpm, playheadSample);

    const int patternLen = drumPattern.getLengthInSamples (bpm, currentSampleRate);
    playheadSample = (playheadSample + numSamples) % juce::jmax (1, patternLen);
}
