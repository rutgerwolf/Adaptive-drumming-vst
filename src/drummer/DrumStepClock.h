#pragma once
#include "DrumPattern.h"

#include <cmath>
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
 * *start* of this block (playheadSample == 0), and any triggers past a bar
 * wrap inside the block are hashed with the incremented index — so the
 * groove never depends on how the host happens to slice bars into blocks.
 *
 * Boundary positions are computed directly off the exact (fractional) bar
 * length (DrumPattern::getLengthInSamplesExact) rather than scanned per
 * sample against an integer-truncated length: step k of bar b sits at
 * llround((16*b + k) * patternLenExact / 16), walked forward with a cursor
 * instead of tested with a per-sample modulo. This kills the old scheme's
 * two artefacts — a spurious boundary at the truncation remainder past step
 * 15 (the "phantom 17th step"), and up to 15 samples of the bar's tail
 * falling outside the 16-step grid entirely (the grid "compressing" into
 * 16 * (patternLen / 16) samples instead of the true bar length) — and is
 * cheaper besides: O(steps in range) rather than O(numSamples).
 *
 * The active style's swing (GrooveTable::swing, 0..255) delays odd-indexed
 * 16th-note boundaries by up to 2/3 of a step before rounding; even-indexed
 * boundaries are never moved. All authored styles currently carry swing 0
 * (an exact no-op), so today's triggers land exactly where they always have
 * — this is the seam a later swung style (e.g. Jazz) plugs into without
 * touching this file again.
 *
 * voiceIndex matches the DrumPattern voice bit order:
 *   0 Kick · 1 Snare · 2 HiHat · 3 Crash · 4 Ride · 5 Tom
 */

/** Sample position of global 16th-note boundary `globalStep` (step k of bar
    b is globalStep == 16*b + k, b,k >= 0), given the exact per-step length
    and the active style's swing amount. Odd-indexed steps within a bar are
    delayed by up to swing01 * stepLenExact * (2/3); even-indexed steps are
    never moved. Exposed standalone (not just via forEachStepTrigger below)
    so swing behaviour is directly unit-testable without authoring a swung
    style table. Monotonically non-decreasing in globalStep for swing01 in
    [0, 1] (the swing offset never exceeds a step length). */
inline long long stepBoundarySample (long long globalStep, double stepLenExact, double swing01) noexcept
{
    const int localStep = static_cast<int> (((globalStep % DrumPattern::kSteps) + DrumPattern::kSteps)
                                             % DrumPattern::kSteps);
    double pos = static_cast<double> (globalStep) * stepLenExact;
    if ((localStep & 1) != 0)
        pos += swing01 * stepLenExact * (2.0 / 3.0);
    return static_cast<long long> (std::llround (pos));
}

template <typename OnTrigger>
inline void forEachStepTrigger (const DrumPattern& pattern,
                                int                playheadSample,
                                int                numSamples,
                                double             bpm,
                                double             sampleRate,
                                uint32_t           barIndexAtBlockStart,
                                OnTrigger&&        onTrigger)
{
    if (numSamples <= 0 || playheadSample < 0) return;

    const double patternLenExact = pattern.getLengthInSamplesExact (bpm, sampleRate);
    if (patternLenExact <= 0.0) return;

    const double stepLenExact = patternLenExact / static_cast<double> (DrumPattern::kSteps);
    if (stepLenExact <= 0.0) return;

    const double swing01 = static_cast<double> (pattern.getActiveTable().swing) / 255.0;

    const long long rangeStart = playheadSample;
    const long long rangeEnd   = playheadSample + static_cast<long long> (numSamples);   // exclusive

    // Find the first global step boundary at or after rangeStart: a coarse
    // estimate from the (swing-free) average step rate, then walk the cursor
    // to the exact answer — at most a couple of steps of correction, since
    // swing can shift a boundary by at most 2/3 of one step.
    long long n = static_cast<long long> (std::floor (static_cast<double> (rangeStart) / stepLenExact));
    if (n > 0) --n;
    while (stepBoundarySample (n, stepLenExact, swing01) < rangeStart)
        ++n;
    while (n > 0 && stepBoundarySample (n - 1, stepLenExact, swing01) >= rangeStart)
        --n;

    for (; ; ++n)
    {
        const long long pos = stepBoundarySample (n, stepLenExact, swing01);
        if (pos >= rangeEnd)
            break;

        const int      offset = static_cast<int> (pos - rangeStart);
        const long long bar   = n / DrumPattern::kSteps;
        const int       step  = static_cast<int> (n % DrumPattern::kSteps);
        const uint32_t  barIdx = barIndexAtBlockStart + static_cast<uint32_t> (bar);

        for (int voice = 0; voice < DrumPattern::kNumVoices; ++voice)
            if (pattern.stepFires (voice, step, barIdx))
                onTrigger (voice, offset, pattern.stepVelocity (voice, step, barIdx));
    }
}
