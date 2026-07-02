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

AdaptiveDrummer::AdaptiveDrummer() = default;   // DrumPattern ctor loads Rock/Medium

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
    barIndex             = 0;
    drumSampler.reset();
    drumSynth.reset();
    // A pending style request deliberately survives reset: the playhead is now
    // at step 0, so the next processBlock() applies it before the first trigger.
}

void AdaptiveDrummer::setStyle (DrumPattern::Style style)
{
    // Record only — applied at the next step-0 bar wrap by processBlock().
    // Safe (and cheap) to call every block: no pattern rebuild happens here,
    // which is what used to make the per-block setStyle a hazard (finding A2).
    pendingStyleIdx.store (static_cast<int> (style), std::memory_order_release);
}

void AdaptiveDrummer::applyPendingStyle() noexcept
{
    const auto pending = static_cast<DrumPattern::Style> (
        pendingStyleIdx.load (std::memory_order_acquire));

    if (pending != drumPattern.getStyle())
        drumPattern.loadStyle (pending);   // O(1): re-points a const GrooveTable
}

void AdaptiveDrummer::setBpm (double newBpm)
{
    bpm = juce::jlimit (40.0, 240.0, newBpm);
}

void AdaptiveDrummer::setDensity (DrumPattern::Density density)
{
    // Legacy macro onto the 2-D axes (mapLegacy inside DrumPattern). Applied
    // immediately: complexity is sampled per step boundary by design — only
    // *style* switches are bar-latched.
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

void AdaptiveDrummer::renderSegment (juce::AudioBuffer<float>& outBuffer,
                                     int startSample, int numSamples, int posInPattern)
{
    if (numSamples <= 0)
        return;

    // Aliasing view of the block's tail — same sample memory, no copy. The
    // channel-pointer constructor is allocation-free for realistic channel
    // counts (AudioBuffer keeps up to 32 channel pointers inline).
    juce::AudioBuffer<float> view (outBuffer.getArrayOfWritePointers(),
                                   outBuffer.getNumChannels(),
                                   startSample, numSamples);

    if (useSynth)
        drumSynth.processBlock (view, numSamples, currentSampleRate,
                                drumPattern, bpm, posInPattern, barIndex);
    else
        drumSampler.processBlock (view, numSamples, currentSampleRate,
                                  drumPattern, bpm, posInPattern, barIndex);
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
    // (barIndex is deliberately not touched by a snap — it is a hash counter,
    // not a musical position; see the class comment.)
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

    if (patternLen <= 0)
    {
        // No usable timeline (invalid bpm/sample rate): nothing can trigger,
        // but active voices must still decay. Latch any pending style now.
        applyPendingStyle();
        renderSegment (outBuffer, 0, numSamples, 0);
        playheadSample = 0.0;
        return;
    }

    // Render in segments split at bar wraps, so a pending style is applied
    // exactly when the playhead reaches step 0 (never mid-bar) and the bar
    // counter feeding the groove hash advances once per wrap. In the common
    // case (no wrap inside the block) this is a single full-block segment.
    int bufOffset = 0;
    int remaining = numSamples;
    int pos       = intPlayhead;
    while (remaining > 0)
    {
        if (pos == 0)
            applyPendingStyle();

        const int segLen = juce::jmin (remaining, patternLen - pos);
        renderSegment (outBuffer, bufOffset, segLen, pos);

        bufOffset += segLen;
        remaining -= segLen;
        pos       += segLen;
        if (pos >= patternLen)
        {
            pos = 0;
            ++barIndex;
        }
    }

    // Free-run: always advance by exactly numSamples, so consecutive blocks
    // tile the pattern with no gap or overlap.
    playheadSample = std::fmod (playheadSample + static_cast<double> (numSamples),
                                static_cast<double> (patternLen));
}
