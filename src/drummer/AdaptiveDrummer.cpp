#include "AdaptiveDrummer.h"

#include <cmath>

namespace
{
    // How far the free-running playhead may drift from the host's ppq-derived
    // position before snapping to it. Small enough that the drummer stays
    // locked to the DAW bar line over a whole session (prevents long-term
    // tempo drift from the pattern length's integer truncation); large enough
    // that ordinary block-to-block rounding noise never triggers a snap.
    constexpr double kHostResyncToleranceSeconds = 0.002;   // ~2 ms
}

AdaptiveDrummer::AdaptiveDrummer()
{
    drumPattern.loadStyle (DrumPattern::Style::Rock);
}

void AdaptiveDrummer::prepare (double sampleRate, int /*blockSize*/)
{
    currentSampleRate = sampleRate;
    drumSampler.prepare (sampleRate);
    drumSynth.prepare (sampleRate);
    reset();
}

void AdaptiveDrummer::reset()
{
    playheadSample       = 0.0;
    lastRenderedPlayhead = 0;
    drumSampler.reset();
    drumSynth.reset();
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

void AdaptiveDrummer::setHostTimeline (bool isPlaying, bool hasPpqPosition, double ppqPosition) noexcept
{
    hostIsPlaying   = isPlaying;
    hostHasPpq      = hasPpqPosition;
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

    // Gently correct drift against the host's ppq position instead of
    // overwriting the playhead every block. Re-deriving playheadSample from
    // ppq on every block, against a pattern length that can't exactly equal
    // the host's true (fractional-sample) bar length, made consecutive
    // blocks' scan windows overlap or gap by a sample — so a step boundary
    // was scanned twice (a flam) or not at all (a dropped hit). Free-running
    // instead tiles blocks exactly; an occasional, rare, sub-audible snap
    // keeps the drummer locked to the host's bar line over a whole session.
    if (patternLen > 0 && hostIsPlaying && hostHasPpq)
    {
        const double target = static_cast<double> (ppqToPlayhead (hostPpqPosition, patternLen));

        // Shortest signed distance from the free-running playhead to the
        // ppq-derived target, wrapped into the pattern's circular space.
        double diff = target - playheadSample;
        diff -= static_cast<double> (patternLen) * std::round (diff / static_cast<double> (patternLen));

        const double toleranceSamples = kHostResyncToleranceSeconds * currentSampleRate;
        if (std::abs (diff) > toleranceSamples)
            playheadSample = target;
    }

    const int intPlayhead = patternLen > 0
        ? static_cast<int> (static_cast<juce::int64> (std::llround (playheadSample)) % patternLen)
        : 0;
    lastRenderedPlayhead = intPlayhead;

    if (useSynth)
        drumSynth.processBlock (outBuffer, numSamples, currentSampleRate,
                                drumPattern, bpm, intPlayhead);
    else
        drumSampler.processBlock (outBuffer, numSamples, currentSampleRate,
                                  drumPattern, bpm, intPlayhead);

    // Free-run: always advance by exactly numSamples, so consecutive blocks
    // tile the pattern with no gap or overlap.
    playheadSample = patternLen > 0
        ? std::fmod (playheadSample + static_cast<double> (numSamples), static_cast<double> (patternLen))
        : 0.0;
}
