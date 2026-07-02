#pragma once
#include "DrumPattern.h"

#include <cstdint>

/**
 * Shared step-trigger timing for the sound engines.
 *
 * Walks the samples in a process block and calls
 * onTrigger(voiceIndex, offsetInBlock, velocity01) for every voice hit whose
 * 16th-note step boundary falls inside the block. Both DrumSampler and
 * DrumSynth use this so their timing is identical and sample-accurate.
 *
 * Which voices fire at a boundary — and how hard — is the pattern's per-cell
 * decision (DrumPattern::stepFires / stepVelocity: complexity threshold gate,
 * seeded-hash probability, intensity-lerped velocity). The decision is keyed
 * on the bar index so probabilistic cells vary per bar yet stay bit-exactly
 * reproducible: barIndexAtBlockStart is the caller's bar counter for the
 * *start* of this block, and any triggers past a bar wrap inside the block
 * are hashed with the incremented index — so the groove never depends on how
 * the host happens to slice bars into blocks.
 *
 * NOTE (deliberate): the boundary *detection* below still scans per sample
 * against the integer-truncated pattern/step lengths, exactly as before the
 * 2-D model landed. The exact-double boundary rewrite (kills the phantom
 * step-16 remainder scan and the up-to-15-sample grid compression, and adds
 * swing consumption — finding #11/B4, low severity) is separate deferred
 * work; do not fold it into pattern-model changes. stepFires() returns false
 * for step 16, preserving the old getStep(16) == 0 behaviour.
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
                                uint32_t           barIndexAtBlockStart,
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
            const int      step   = posInPattern / stepLen;
            const uint32_t barIdx = barIndexAtBlockStart
                + static_cast<uint32_t> ((playheadSample + offset) / patternLen);

            for (int voice = 0; voice < DrumPattern::kNumVoices; ++voice)
                if (pattern.stepFires (voice, step, barIdx))
                    onTrigger (voice, offset, pattern.stepVelocity (voice, step, barIdx));
        }
    }
}
