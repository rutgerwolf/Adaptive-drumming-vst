#pragma once
#include <cstdint>

/**
 * GrooveHash — stateless, seedable hashing for per-trigger groove decisions.
 *
 * The 2-D engine's probabilistic layer (StepCell::probability < 255) and
 * velocity humanization (StepCell::velRandRange) must be random-sounding yet
 * bit-exactly reproducible: realtime playback == offline bounce == session
 * reload for the same groove seed. So there is no RNG object and no mutable
 * state anywhere — every decision is a pure SplitMix64-style avalanche of
 * (seed, barIndex, voice, step, stream), computed inline on the audio thread
 * (a handful of integer multiplies/shifts per trigger; no allocation, no
 * locks, no juce::Random).
 *
 * `stream` decorrelates the different uses of the hash at one trigger
 * (stream 0 = fire/probability draw, stream 1 = velocity humanization).
 */
namespace groove
{

/** SplitMix64 avalanche step — statistically strong mixing of 64 bits. */
constexpr uint64_t splitMix64 (uint64_t z) noexcept
{
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/** 64 uniform bits from the packed trigger identity.
    voice < 16 and step < 16 occupy distinct nibbles, so every
    (voice, step, stream < 16) combination XORs a distinct mask. */
constexpr uint64_t triggerHash (uint32_t seed, uint32_t barIndex,
                                uint32_t voice, uint32_t step, uint32_t stream) noexcept
{
    uint64_t h = (static_cast<uint64_t> (seed) << 32) | barIndex;
    h = splitMix64 (h);
    h ^= (static_cast<uint64_t> (voice) << 8)
       | (static_cast<uint64_t> (step)  << 4)
       |  static_cast<uint64_t> (stream);
    return splitMix64 (h);
}

/** Uniform float in [0, 1) — top 24 bits, exactly representable. */
constexpr float hash01 (uint32_t seed, uint32_t barIndex,
                        uint32_t voice, uint32_t step, uint32_t stream = 0) noexcept
{
    return static_cast<float> (triggerHash (seed, barIndex, voice, step, stream) >> 40)
           * (1.0f / 16777216.0f);
}

/** Uniform float in [-1, 1). */
constexpr float hashBipolar (uint32_t seed, uint32_t barIndex,
                             uint32_t voice, uint32_t step, uint32_t stream) noexcept
{
    return hash01 (seed, barIndex, voice, step, stream) * 2.0f - 1.0f;
}

} // namespace groove
