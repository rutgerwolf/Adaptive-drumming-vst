#pragma once
#include <JuceHeader.h>

/**
 * DrumPattern — 16-step drum grid in three styles.
 *
 * Each style has three density layers (Sparse / Medium / Full) that map to
 * the GarageBand Drummer concept of soft/simple → loud/complex.
 *
 * Voice bits: Kick=0x01 Snare=0x02 HiHat=0x04 Crash=0x08 Ride=0x10 Tom=0x20
 */
class DrumPattern
{
public:
    enum class Style   { Rock, Jazz, Electronic };
    enum class Density { Sparse, Medium, Full };

    enum Voice : uint8_t
    {
        Kick   = 1 << 0,   // 0x01
        Snare  = 1 << 1,   // 0x02
        HiHat  = 1 << 2,   // 0x04
        Crash  = 1 << 3,   // 0x08
        Ride   = 1 << 4,   // 0x10
        Tom    = 1 << 5,   // 0x20
    };

    static constexpr int kSteps = 16;  // 16th-note resolution, 1 bar

    DrumPattern();

    void loadStyle  (Style style);
    void setDensity (Density density);

    Style   getStyle()   const noexcept { return currentStyle; }
    Density getDensity() const noexcept { return currentDensity; }

    /** Returns the voice bitmask for step [0, kSteps). */
    uint8_t getStep (int stepIndex) const;

    /** Pattern length in samples for the given tempo and sample rate (one bar). */
    int getLengthInSamples (double bpm, double sampleRate) const;

private:
    void buildRock();
    void buildJazz();
    void buildElectronic();

    uint8_t layers[3][kSteps] {};   // [density][step]
    Style   currentStyle   { Style::Rock };
    Density currentDensity { Density::Medium };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumPattern)
};
