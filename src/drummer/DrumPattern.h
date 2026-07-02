#pragma once
#include <JuceHeader.h>

#include <cstdint>

/**
 * DrumPattern — the 2-D (intensity × complexity) groove model.
 *
 * Data model (docs/ARCHITECTURE_2D_FOLLOW.md §2): each style is one immutable,
 * const GrooveTable of per-voice, per-step StepCells. Two continuous axes
 * replace the raw on/off masks the class used to store:
 *
 *   - complexity (0..1, structural): a cell sounds iff
 *     complexity255 >= cell.complexityThreshold, further gated by the cell's
 *     probability byte through a stateless seeded hash (GrooveHash.h). The
 *     deterministic backbone is authored at probability 255.
 *   - intensity (0..1, dynamics): per-hit velocity is
 *     lerp(velLow, velHigh, intensity^0.7) — authored spans re-voice rather
 *     than merely scale (a ghost stays a ghost at full intensity).
 *
 * Legacy 3-step density (Sparse/Medium/Full) is kept as a public API and maps
 * onto the axes via mapLegacy(); the tables are seeded mask-equivalently, so
 * the deterministic backbone at each legacy density point reproduces the old
 * layer masks exactly. (Host-visible intensity/complexity parameters arrive
 * in a later roadmap step; until then setDensity() is the only driver.)
 *
 * RT-safety: all tables are const file-scope data; loadStyle() only re-points
 * a pointer. stepFires()/stepVelocity() are pure arithmetic per trigger — no
 * allocation, no locks, no RNG state.
 */

//==============================================================================
/** One authored cell of a groove table (POD, 6 bytes). */
struct StepCell
{
    uint8_t velLow;               ///< velocity at intensity 0 (0..127)
    uint8_t velHigh;              ///< velocity at intensity 1 (0..127); 0 = cell never plays
    uint8_t complexityThreshold;  ///< cell is eligible iff complexity255 >= this
    uint8_t probability;          ///< 0..255; 255 = deterministic backbone, < 255 = hash-gated
    int8_t  velRandRange;         ///< ± humanization (velocity units) via the seeded hash
    uint8_t flags;                ///< see kFlag* below (informational; reserved for later steps)

    static constexpr uint8_t kFlagAccent   = 1 << 0;
    static constexpr uint8_t kFlagGhost    = 1 << 1;
    static constexpr uint8_t kFlagFillOnly = 1 << 2;   // reserved
};

/** One style's complete groove: 6 voices × 16 steps of StepCells. */
struct GrooveTable
{
    static constexpr int kNumVoices = 6;
    static constexpr int kNumSteps  = 16;

    StepCell cells[kNumVoices][kNumSteps];
    uint8_t  swing;      ///< 0..255 → 0..~66% delay of odd 16ths. Authored but not yet
                         ///< consumed: the clock's boundary math is deliberately untouched
                         ///< until the exact-double boundary rewrite lands (all styles 0).
    uint32_t styleSalt;  ///< mixed into the trigger hash so styles decorrelate
};

static_assert (sizeof (StepCell) == 6, "StepCell must stay a tightly-packed POD");

//==============================================================================
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

    static constexpr int kSteps     = GrooveTable::kNumSteps;   // 16th-note resolution, 1 bar
    static constexpr int kNumVoices = GrooveTable::kNumVoices;

    /** Fixed default groove seed. Becomes user-editable state in a later
        roadmap step; until then every instance hashes with this constant. */
    static constexpr uint32_t kDefaultGrooveSeed = 0x67726F76u;   // 'grov'

    DrumPattern();

    /** Selects a style's const GrooveTable (O(1) pointer re-point, RT-safe). */
    void loadStyle  (Style style);

    /** Legacy 3-step density macro: drives both axes through mapLegacy().
        Kept API-stable for EnergyAnalyzer / processor / editor callers. */
    void setDensity (Density density);

    Style   getStyle()   const noexcept { return currentStyle; }
    Density getDensity() const noexcept { return currentDensity; }

    // ── 2-D axes ──────────────────────────────────────────────────────────────
    /** Structural axis (0..1): which cells are active. Sampled per trigger. */
    void  setComplexity (float complexity01) noexcept;
    /** Dynamics axis (0..1): per-hit velocity re-voicing. */
    void  setIntensity  (float intensity01) noexcept;
    float getComplexity () const noexcept { return complexity; }
    float getIntensity  () const noexcept { return intensity; }

    /** The legacy density → (complexity, intensity) mapping (mask-equivalent
        points; see the table seeding in DrumPattern.cpp). */
    static void mapLegacy (Density density,
                           float& complexity01, float& intensity01) noexcept;

    void     setSeed (uint32_t newSeed) noexcept { grooveSeed = newSeed; }
    uint32_t getSeed () const noexcept           { return grooveSeed; }

    // ── Per-trigger queries (audio thread; pure arithmetic) ───────────────────
    /** True iff (voice, step) sounds in bar barIndex at the current complexity
        and seed. Out-of-range voice/step returns false — this deliberately
        keeps the old getStep() bounds behaviour for the clock's latent
        step-16 remainder scan (finding #11, deferred). */
    bool  stepFires    (int voice, int step, uint32_t barIndex) const noexcept;

    /** Velocity 0..1 for a firing (voice, step) in bar barIndex at the current
        intensity, including the cell's hash humanization. 0 if out of range. */
    float stepVelocity (int voice, int step, uint32_t barIndex) const noexcept;

    /** The style's immutable table (exposed for tests / future editors). */
    const GrooveTable& getActiveTable() const noexcept { return *activeTable; }

    /** Pattern length in samples for the given tempo and sample rate (one bar). */
    int getLengthInSamples (double bpm, double sampleRate) const;

private:
    const GrooveTable* activeTable;             // one of the const style tables
    Style    currentStyle   { Style::Rock };
    Density  currentDensity { Density::Medium };

    // Sentinels: the ctor's setDensity(Medium) performs the real initialisation
    // (the setters early-out when the value is unchanged, so the initial value
    // must never equal a settable one).
    float    complexity      { -1.0f };
    float    intensity       { -1.0f };
    uint8_t  complexity255   { 0 };             // complexity scaled to the threshold domain
    float    intensityShaped { 0.0f };          // intensity^0.7, precomputed per set
    uint32_t grooveSeed      { kDefaultGrooveSeed };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumPattern)
};
