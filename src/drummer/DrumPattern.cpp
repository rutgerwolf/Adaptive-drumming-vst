#include "DrumPattern.h"
#include "GrooveHash.h"

#include <cmath>

// ── The style tables ──────────────────────────────────────────────────────────
//
// Mask-equivalent seeding (the migration invariant, ARCHITECTURE_2D_FOLLOW §2.1):
// every hit of the legacy Sparse layer is authored at threshold kThrSparse (0),
// hits the legacy Medium layer added at kThrMedium (100), hits the legacy Full
// layer added at kThrFull (190) — all at probability 255. With
//   mapLegacy: Sparse → complexity 0.20 (c255 =  51)
//              Medium → complexity 0.55 (c255 = 140)
//              Full   → complexity 1.00 (c255 = 255)
// the deterministic backbone firing at each legacy density point is exactly the
// old layers[density][step] bitmask (golden-tested in DrumPatternTest).
//
// New musical territory sits above the legacy range: ghost/ornament cells at
// thresholds 200–240 with probability < 255 and velRandRange humanization.
// They only become audible near full complexity, thicken with it (probScale),
// and vary per bar through the stateless seeded hash.

namespace
{
    constexpr uint8_t kThrSparse = 0;     // legacy Sparse backbone
    constexpr uint8_t kThrMedium = 100;   // added by legacy Medium
    constexpr uint8_t kThrFull   = 190;   // added by legacy Full

    // Voice rows (DrumPattern::Voice bit order).
    constexpr int vKick = 0, vSnare = 1, vHiHat = 2, vCrash = 3, vRide = 4, vTom = 5;

    constexpr StepCell hit (uint8_t velLow, uint8_t velHigh, uint8_t threshold,
                            uint8_t flags = 0) noexcept
    {
        return { velLow, velHigh, threshold, 255, 0, flags };
    }

    constexpr StepCell ghost (uint8_t velLow, uint8_t velHigh, uint8_t threshold,
                              uint8_t probability, int8_t velRand) noexcept
    {
        return { velLow, velHigh, threshold, probability, velRand, StepCell::kFlagGhost };
    }

    // Gently raises ornament probability with complexity so the probabilistic
    // layer thickens along the same axis as the threshold layer. Monotone
    // non-decreasing in complexity — required for the hit-count-monotonic
    // invariant (a cell that fires at complexity c must fire at c' > c for the
    // same seed/bar).
    constexpr float probScale (float complexity01) noexcept
    {
        return 0.5f + 0.5f * complexity01;
    }

    constexpr GrooveTable makeRock() noexcept
    {
        GrooveTable t {};   // velHigh == 0 everywhere → silent unless authored
        t.swing     = 0;
        t.styleSalt = 0x726F636Bu;   // 'rock'

        // Kick — 1 & 3 backbone, Full adds the "and" pushes.
        t.cells[vKick][ 0] = hit (98, 127, kThrSparse, StepCell::kFlagAccent);
        t.cells[vKick][ 8] = hit (95, 124, kThrSparse);
        t.cells[vKick][ 2] = hit (78, 112, kThrFull);
        t.cells[vKick][10] = hit (80, 114, kThrFull);

        // Snare — 2 & 4 backbeat, plus hash-gated ghosts near full complexity.
        t.cells[vSnare][ 4] = hit (92, 127, kThrSparse, StepCell::kFlagAccent);
        t.cells[vSnare][12] = hit (92, 127, kThrSparse, StepCell::kFlagAccent);
        t.cells[vSnare][ 7] = ghost (10, 26, 205, 150, 8);
        t.cells[vSnare][15] = ghost (10, 24, 220, 130, 8);

        // HiHat — quarters → 8ths → 16ths as complexity rises; the odd-16th
        // ticks are authored quiet with a wide span (tick → bark on intensity).
        for (int s = 0; s < GrooveTable::kNumSteps; ++s)
        {
            if      (s % 4 == 0) t.cells[vHiHat][s] = hit (55, 105, kThrSparse);
            else if (s % 2 == 0) t.cells[vHiHat][s] = hit (42,  92, kThrMedium);
            else                 t.cells[vHiHat][s] = hit (25,  72, kThrFull);
        }

        // Crash — downbeat only, full complexity only.
        t.cells[vCrash][0] = hit (95, 127, kThrFull, StepCell::kFlagAccent);

        // Tom — the little fill into beat 4.
        t.cells[vTom][13] = hit (72, 112, kThrFull);
        t.cells[vTom][14] = hit (76, 116, kThrFull);

        return t;
    }

    constexpr GrooveTable makeJazz() noexcept
    {
        GrooveTable t {};
        t.swing     = 0;             // still straight — the swung re-author is a later step
        t.styleSalt = 0x6A617A7Au;   // 'jazz'

        // Kick — feathered; more syncopation with complexity.
        t.cells[vKick][ 0] = hit (85, 115, kThrSparse);
        t.cells[vKick][ 8] = hit (78, 108, kThrMedium);
        t.cells[vKick][11] = hit (72, 104, kThrFull);

        // Snare — 2 & 4.
        t.cells[vSnare][ 4] = hit (85, 120, kThrSparse);
        t.cells[vSnare][12] = hit (85, 120, kThrSparse);

        // Ride — quarters → 8ths → 16ths.
        for (int s = 0; s < GrooveTable::kNumSteps; ++s)
        {
            if      (s % 4 == 0) t.cells[vRide][s] = hit (62, 110, kThrSparse);
            else if (s % 2 == 0) t.cells[vRide][s] = hit (48,  95, kThrMedium);
            else                 t.cells[vRide][s] = hit (28,  70, kThrFull);
        }

        // Tom — fill into beat 4.
        t.cells[vTom][13] = hit (68, 108, kThrFull);
        t.cells[vTom][14] = hit (72, 112, kThrFull);

        return t;
    }

    constexpr GrooveTable makeElectronic() noexcept
    {
        GrooveTable t {};
        t.swing     = 0;
        t.styleSalt = 0x656C6563u;   // 'elec'

        // Kick — four-on-the-floor, uniform by design (machine feel); Full
        // adds the off-beat push on step 10.
        t.cells[vKick][ 0] = hit (92, 127, kThrSparse, StepCell::kFlagAccent);
        t.cells[vKick][ 4] = hit (92, 127, kThrSparse);
        t.cells[vKick][ 8] = hit (92, 127, kThrSparse);
        t.cells[vKick][12] = hit (92, 127, kThrSparse);
        t.cells[vKick][10] = hit (85, 118, kThrFull);

        // Snare — 2 & 4, plus hash-gated end-of-bar ghost fills.
        t.cells[vSnare][ 4] = hit (90, 125, kThrSparse);
        t.cells[vSnare][12] = hit (90, 125, kThrSparse);
        t.cells[vSnare][14] = ghost (12, 28, 210, 140, 10);
        t.cells[vSnare][15] = ghost (12, 26, 235, 110, 10);

        // HiHat — 8ths from Medium, 16ths at Full (legacy Sparse had none).
        for (int s = 0; s < GrooveTable::kNumSteps; ++s)
        {
            if (s % 2 == 0) t.cells[vHiHat][s] = hit (45, 95, kThrMedium);
            else            t.cells[vHiHat][s] = hit (24, 68, kThrFull);
        }

        // Crash — downbeat, full complexity only.
        t.cells[vCrash][0] = hit (95, 127, kThrFull, StepCell::kFlagAccent);

        return t;
    }

    constexpr GrooveTable kRockTable       = makeRock();
    constexpr GrooveTable kJazzTable       = makeJazz();
    constexpr GrooveTable kElectronicTable = makeElectronic();

    constexpr const GrooveTable* kStyleTables[] = { &kRockTable, &kJazzTable, &kElectronicTable };
}

// ── DrumPattern ───────────────────────────────────────────────────────────────

DrumPattern::DrumPattern()
    : activeTable (&kRockTable)
{
    loadStyle  (Style::Rock);
    setDensity (Density::Medium);   // initialises both axes via mapLegacy
}

void DrumPattern::loadStyle (Style style)
{
    currentStyle = style;
    activeTable  = kStyleTables[static_cast<int> (style)];   // O(1), RT-safe
}

void DrumPattern::mapLegacy (Density density,
                             float& complexity01, float& intensity01) noexcept
{
    switch (density)
    {
        case Density::Sparse:  complexity01 = 0.20f; intensity01 = 0.35f; break;
        default:
        case Density::Medium:  complexity01 = 0.55f; intensity01 = 0.55f; break;
        case Density::Full:    complexity01 = 1.00f; intensity01 = 0.85f; break;
    }
}

void DrumPattern::setDensity (Density density)
{
    currentDensity = density;

    float c = 0.0f, i = 0.0f;
    mapLegacy (density, c, i);
    setComplexity (c);
    setIntensity  (i);
}

void DrumPattern::setComplexity (float complexity01) noexcept
{
    const float c = juce::jlimit (0.0f, 1.0f, complexity01);
    if (juce::exactlyEqual (c, complexity)) return;   // called per block — skip the rescale when unchanged

    complexity    = c;
    complexity255 = static_cast<uint8_t> (c * 255.0f + 0.5f);
}

void DrumPattern::setIntensity (float intensity01) noexcept
{
    const float i = juce::jlimit (0.0f, 1.0f, intensity01);
    if (juce::exactlyEqual (i, intensity)) return;    // called per block — skip the pow when unchanged

    intensity       = i;
    intensityShaped = std::pow (i, 0.7f);   // per-set, not per-trigger
}

bool DrumPattern::stepFires (int voice, int step, uint32_t barIndex) const noexcept
{
    if (voice < 0 || voice >= kNumVoices || step < 0 || step >= kSteps)
        return false;   // keeps the old getStep() bounds behaviour (finding #11 stays latent-safe)

    const StepCell& cell = activeTable->cells[voice][step];

    if (cell.velHigh == 0 || complexity255 < cell.complexityThreshold)
        return false;

    if (cell.probability == 255)
        return true;    // deterministic backbone

    const float draw = groove::hash01 (grooveSeed ^ activeTable->styleSalt, barIndex,
                                       static_cast<uint32_t> (voice),
                                       static_cast<uint32_t> (step), 0);
    return draw * 255.0f < static_cast<float> (cell.probability) * probScale (complexity);
}

float DrumPattern::stepVelocity (int voice, int step, uint32_t barIndex) const noexcept
{
    if (voice < 0 || voice >= kNumVoices || step < 0 || step >= kSteps)
        return 0.0f;

    const StepCell& cell = activeTable->cells[voice][step];

    float vel = (static_cast<float> (cell.velLow)
                 + static_cast<float> (cell.velHigh - cell.velLow) * intensityShaped)
                / 127.0f;

    if (cell.velRandRange != 0)
        vel += groove::hashBipolar (grooveSeed ^ activeTable->styleSalt, barIndex,
                                    static_cast<uint32_t> (voice),
                                    static_cast<uint32_t> (step), 1)
               * static_cast<float> (cell.velRandRange) / 127.0f;

    return juce::jlimit (0.0f, 1.0f, vel);
}

int DrumPattern::getLengthInSamples (double bpm, double sampleRate) const
{
    if (bpm <= 0.0 || sampleRate <= 0.0) return 0;
    return static_cast<int> ((4.0 * 60.0 / bpm) * sampleRate);
}
