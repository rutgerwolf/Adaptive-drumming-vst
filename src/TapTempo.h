#pragma once
#include <JuceHeader.h>

/**
 * TapTempo — pure tap-tempo BPM math, used by AdaptiveDrummerEditor's TAP
 * button. Separated from the editor so it's directly unit-testable without
 * instantiating a GUI component (this codebase's tests never call
 * createEditor(); see PluginEditor.h's class comment).
 */
namespace TapTempo
{
    /** BPM implied by a sequence of tap timestamps in milliseconds (e.g. from
        juce::Time::getMillisecondCounterHiRes()), averaged across the whole
        span rather than the last interval alone — smooths a single jittery
        tap instead of letting it dominate the estimate. Returns 0.0 (no
        result) if fewer than 2 timestamps are given, or the span isn't
        strictly positive (duplicate or non-increasing timestamps). */
    inline double computeBpm (const juce::Array<double>& tapTimestampsMs) noexcept
    {
        if (tapTimestampsMs.size() < 2)
            return 0.0;

        const double spanMs = tapTimestampsMs.getLast() - tapTimestampsMs.getFirst();
        if (spanMs <= 0.0)
            return 0.0;

        const double intervalMs = spanMs / static_cast<double> (tapTimestampsMs.size() - 1);
        return 60000.0 / intervalMs;
    }
}
