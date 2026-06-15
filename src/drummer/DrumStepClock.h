#pragma once
#include "DrumPattern.h"

#include <cstdint>

/**
 * Shared step-trigger timing for the sound engines.
 *
 * Walks the samples in a process block and calls onTrigger(voiceIndex, offset)
 * for every voice hit whose 16th-note step boundary falls inside the block.
 * Both DrumSampler and DrumSynth use this so their timing is identical and
 * sample-accurate.
 *
 * voiceIndex matches the DrumPattern voice bit order:
 *   0 Kick · 1 Snare · 2 HiHat · 3 Crash · 4 Ride · 5 Tom
 */
template <typename OnTrigger>
inline void forEachStepTrigger (const DrumPattern& pattern,
                                int                playheadSample,
                                int                numSamples,
                                double             bpm,
                                double             sampleRate,
                                OnTrigger&&        onTrigger)
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
            const uint8_t mask = pattern.getStep (posInPattern / stepLen);

            for (int voice = 0; voice < 6; ++voice)
                if (mask & (1u << voice))
                    onTrigger (voice, offset);
        }
    }
}
