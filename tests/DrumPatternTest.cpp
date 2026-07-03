#include <JuceHeader.h>
#include "drummer/DrumPattern.h"
#include "drummer/GrooveHash.h"

#include <cmath>

/**
 * Regression net for the 2-D groove model (StepCell/GrooveTable).
 *
 * Restructured per Roadmap 2D steps 9+11: instead of pinning raw bitmasks,
 * these tests pin the *structural invariants* the model must keep under any
 * re-authoring (backbone placement, monotonicity in complexity, superset
 * property, crash gating), plus the migration contract itself: at the three
 * legacy density points the deterministic (probability-255) backbone must
 * reproduce the exact pre-GrooveTable layer masks, which are hardcoded here
 * as golden data.
 */
class DrumPatternTest : public juce::UnitTest
{
public:
    DrumPatternTest() : juce::UnitTest ("DrumPattern", "drummer") {}

    static constexpr int kVoices = DrumPattern::kNumVoices;

    void runTest() override
    {
        const DrumPattern::Style allStyles[] = {
            DrumPattern::Style::Rock, DrumPattern::Style::Jazz, DrumPattern::Style::Electronic };
        const DrumPattern::Density allDensities[] = {
            DrumPattern::Density::Sparse, DrumPattern::Density::Medium, DrumPattern::Density::Full };

        beginTest ("public constants and voice bits are stable");
        {
            expectEquals (DrumPattern::kSteps, 16);
            expectEquals (DrumPattern::kNumVoices, 6);
            expectEquals ((int) DrumPattern::Kick,  0x01);
            expectEquals ((int) DrumPattern::Snare, 0x02);
            expectEquals ((int) DrumPattern::HiHat, 0x04);
            expectEquals ((int) DrumPattern::Crash, 0x08);
            expectEquals ((int) DrumPattern::Ride,  0x10);
            expectEquals ((int) DrumPattern::Tom,   0x20);
        }

        beginTest ("getLengthInSamples — one bar of 4 beats");
        {
            DrumPattern p;
            // 4 beats at 120 BPM = 2.0 s → 88200 samples @ 44.1 kHz
            expectEquals (p.getLengthInSamples (120.0, 44100.0), 88200);
            // 4 beats at 60 BPM = 4.0 s → 192000 samples @ 48 kHz
            expectEquals (p.getLengthInSamples (60.0, 48000.0), 192000);
        }

        beginTest ("getLengthInSamples — invalid inputs return 0");
        {
            DrumPattern p;
            expectEquals (p.getLengthInSamples (0.0,   44100.0), 0);
            expectEquals (p.getLengthInSamples (-1.0,  44100.0), 0);
            expectEquals (p.getLengthInSamples (120.0, 0.0),     0);
            expectEquals (p.getLengthInSamples (120.0, -1.0),    0);
        }

        beginTest ("out-of-range queries are silent and safe");
        {
            // Same contract the old getStep() had: anything outside the grid is
            // a non-hit. The step-16 case in particular guards the clock's
            // latent remainder scan (finding #11, deferred).
            DrumPattern p;
            p.setDensity (DrumPattern::Density::Full);
            for (int bar : { 0, 1, 7 })
            {
                expect (! p.stepFires (0, -1, (uint32_t) bar));
                expect (! p.stepFires (0, DrumPattern::kSteps, (uint32_t) bar));
                expect (! p.stepFires (0, 10000, (uint32_t) bar));
                expect (! p.stepFires (-1, 0, (uint32_t) bar));
                expect (! p.stepFires (kVoices, 0, (uint32_t) bar));
                expectEquals (p.stepVelocity (0, DrumPattern::kSteps, (uint32_t) bar), 0.0f);
                expectEquals (p.stepVelocity (-1, 0, (uint32_t) bar), 0.0f);
            }
        }

        beginTest ("legacy density API — setDensity/getDensity round-trip and axis mapping");
        {
            DrumPattern p;
            expect (p.getDensity() == DrumPattern::Density::Medium, "default density is Medium");

            for (auto d : allDensities)
            {
                p.setDensity (d);
                expect (p.getDensity() == d, "getDensity must return the value set");

                float c = -1.0f, i = -1.0f;
                DrumPattern::mapLegacy (d, c, i);
                expectWithinAbsoluteError (p.getComplexity(), c, 1.0e-6f);
                expectWithinAbsoluteError (p.getIntensity(),  i, 1.0e-6f);
            }

            // The documented legacy points themselves.
            float c = 0.0f, i = 0.0f;
            DrumPattern::mapLegacy (DrumPattern::Density::Sparse, c, i);
            expectWithinAbsoluteError (c, 0.20f, 1.0e-6f);
            expectWithinAbsoluteError (i, 0.35f, 1.0e-6f);
            DrumPattern::mapLegacy (DrumPattern::Density::Medium, c, i);
            expectWithinAbsoluteError (c, 0.55f, 1.0e-6f);
            expectWithinAbsoluteError (i, 0.55f, 1.0e-6f);
            DrumPattern::mapLegacy (DrumPattern::Density::Full, c, i);
            expectWithinAbsoluteError (c, 1.00f, 1.0e-6f);
            expectWithinAbsoluteError (i, 0.85f, 1.0e-6f);
        }

        beginTest ("mask-equivalence — legacy density backbone == pre-GrooveTable masks (golden)");
        {
            // The exact layers[density][step] bitmasks the model stored before
            // the GrooveTable landed. The deterministic backbone (probability
            // 255 cells) at each mapLegacy density point must reproduce them —
            // Rock and Electronic are the original pre-GrooveTable masks; Jazz
            // was re-authored at Step 18 (swing + comping + a skip-note ride)
            // and no longer carries a deterministic snare backbeat by design
            // (see the "snare comping" test below), so its golden values below
            // are the new table's own backbone, not the old bitmasks.
            static const uint8_t goldenMasks[3][3][DrumPattern::kSteps] = {
                { // Rock
                  { 0x05,0,0,0, 0x06,0,0,0, 0x05,0,0,0, 0x06,0,0,0 },
                  { 0x05,0,0x04,0, 0x06,0,0x04,0, 0x05,0,0x04,0, 0x06,0,0x04,0 },
                  { 0x0D,0x04,0x05,0x04, 0x06,0x04,0x04,0x04, 0x05,0x04,0x05,0x04, 0x06,0x24,0x24,0x04 } },
                { // Jazz (re-authored, Step 18): kick 0/8, ride skip-pattern
                  // 0/4/6/8/12/14 at Sparse; ride fills the skipped "and"s
                  // (2/10) at Medium; ride fills the remaining 16ths and kick
                  // adds step 11 at Full (Tom joins at 13/14, hence 0x30).
                  { 0x11,0,0,0, 0x10,0,0x10,0, 0x11,0,0,0, 0x10,0,0x10,0 },
                  { 0x11,0,0x10,0, 0x10,0,0x10,0, 0x11,0,0x10,0, 0x10,0,0x10,0 },
                  { 0x11,0x10,0x10,0x10, 0x10,0x10,0x10,0x10, 0x11,0x10,0x10,0x11, 0x10,0x30,0x30,0x10 } },
                { // Electronic
                  { 0x01,0,0,0, 0x03,0,0,0, 0x01,0,0,0, 0x03,0,0,0 },
                  { 0x05,0,0x04,0, 0x07,0,0x04,0, 0x05,0,0x04,0, 0x07,0,0x04,0 },
                  { 0x0D,0x04,0x04,0x04, 0x07,0x04,0x04,0x04, 0x05,0x04,0x05,0x04, 0x07,0x04,0x04,0x04 } },
            };

            for (int si = 0; si < 3; ++si)
                for (int di = 0; di < 3; ++di)
                {
                    DrumPattern p;
                    p.loadStyle  (allStyles[si]);
                    p.setDensity (allDensities[di]);

                    for (int s = 0; s < DrumPattern::kSteps; ++s)
                        expectEquals ((int) backboneMask (p, s), (int) goldenMasks[si][di][s],
                                      styleName (allStyles[si]) + "/" + densityName (allDensities[di])
                                      + " step " + juce::String (s));
                }
        }

        beginTest ("structural invariants — backbone placement (Rock/Medium, Jazz beat 1)");
        {
            DrumPattern rock;
            rock.loadStyle  (DrumPattern::Style::Rock);
            rock.setDensity (DrumPattern::Density::Medium);
            expect (rock.stepFires (0, 0,  0), "kick on beat 1");
            expect (rock.stepFires (0, 8,  0), "kick on beat 3");
            expect (rock.stepFires (1, 4,  0), "snare on beat 2");
            expect (rock.stepFires (1, 12, 0), "snare on beat 4");
            expect (rock.stepFires (2, 0,  0), "hat on beat 1");

            DrumPattern jazz;
            jazz.loadStyle  (DrumPattern::Style::Jazz);
            jazz.setDensity (DrumPattern::Density::Medium);
            expect (jazz.stepFires (4, 0, 0), "Jazz rides on beat 1");

            // Style must actually change the groove.
            bool differ = false;
            for (int s = 0; s < DrumPattern::kSteps && ! differ; ++s)
                for (int v = 0; v < kVoices && ! differ; ++v)
                    if (rock.stepFires (v, s, 0) != jazz.stepFires (v, s, 0))
                        differ = true;
            expect (differ, "Rock and Jazz medium grooves must not be identical");
        }

        beginTest ("Jazz (Step 18) — swings, has no deterministic snare backbeat, but comps somewhere");
        {
            DrumPattern rock, electronic, jazz;
            rock.loadStyle       (DrumPattern::Style::Rock);
            electronic.loadStyle (DrumPattern::Style::Electronic);
            jazz.loadStyle       (DrumPattern::Style::Jazz);

            expectEquals ((int) rock.getActiveTable().swing,       0, "Rock stays straight");
            expectEquals ((int) electronic.getActiveTable().swing, 0, "Electronic stays straight");
            expectGreaterThan ((int) jazz.getActiveTable().swing,  0, "Jazz must swing");

            // No deterministic (probability-255) snare cell anywhere in the Jazz
            // table — unlike Rock/Electronic's marked 2-&-4 backbeat, Jazz comps.
            for (int s = 0; s < DrumPattern::kSteps; ++s)
                expect (jazz.getActiveTable().cells[1][s].probability != 255
                        || jazz.getActiveTable().cells[1][s].velHigh == 0,
                        "Jazz snare must not be an authored deterministic backbone hit (step "
                        + juce::String (s) + ")");

            // But comping is real, not silent: at Full complexity, across enough
            // bars, the snare fires *somewhere* for the default seed.
            jazz.setComplexity (1.0f);
            jazz.setIntensity  (0.7f);
            bool snareEverFired = false;
            for (uint32_t bar = 0; bar < 8 && ! snareEverFired; ++bar)
                for (int s = 0; s < DrumPattern::kSteps && ! snareEverFired; ++s)
                    if (jazz.stepFires (1, s, bar))
                        snareEverFired = true;
            expect (snareEverFired, "Jazz snare comping must fire at least once over 8 bars at Full complexity");
        }

        beginTest ("structural invariants — hit count monotonically non-decreasing in complexity");
        {
            for (auto style : allStyles)
            {
                DrumPattern p;
                p.loadStyle (style);
                p.setIntensity (0.6f);

                for (uint32_t bar : { 0u, 3u })
                {
                    int previous = -1;
                    for (int step10 = 0; step10 <= 10; ++step10)
                    {
                        p.setComplexity ((float) step10 / 10.0f);
                        const int hits = countFires (p, bar);
                        expect (hits >= previous,
                                styleName (style) + ": hits must not decrease (complexity "
                                + juce::String (step10 / 10.0) + ", bar " + juce::String ((int) bar) + ")");
                        previous = hits;
                    }
                }
            }
        }

        beginTest ("structural invariants — full complexity fires a superset of low complexity");
        {
            // Fixed seed and bar: every cell firing at complexity 0.2 must
            // still fire at 1.0 (thresholds gate monotonically; the hash draw
            // is complexity-independent and probScale is non-decreasing).
            for (auto style : allStyles)
            {
                DrumPattern p;
                p.loadStyle (style);

                for (uint32_t bar = 0; bar < 4; ++bar)
                {
                    bool low[kVoices][DrumPattern::kSteps];
                    p.setComplexity (0.2f);
                    for (int v = 0; v < kVoices; ++v)
                        for (int s = 0; s < DrumPattern::kSteps; ++s)
                            low[v][s] = p.stepFires (v, s, bar);

                    p.setComplexity (1.0f);
                    for (int v = 0; v < kVoices; ++v)
                        for (int s = 0; s < DrumPattern::kSteps; ++s)
                            if (low[v][s])
                                expect (p.stepFires (v, s, bar),
                                        styleName (style) + ": hit at complexity 0.2 lost at 1.0 (voice "
                                        + juce::String (v) + ", step " + juce::String (s) + ")");
                }
            }
        }

        beginTest ("structural invariants — crash only ever on step 0; every density has a kick");
        {
            for (auto style : allStyles)
            {
                DrumPattern p;
                p.loadStyle (style);

                // Exhaustive: the authored table must not contain a playable
                // crash cell anywhere but the downbeat (holds for any
                // complexity/seed/bar, stronger than sampling stepFires).
                for (int s = 1; s < DrumPattern::kSteps; ++s)
                    expectEquals ((int) p.getActiveTable().cells[3][s].velHigh, 0,
                                  styleName (style) + ": crash authored off the downbeat (step "
                                  + juce::String (s) + ")");

                for (auto d : allDensities)
                {
                    p.setDensity (d);
                    bool anyKick = false;
                    for (int s = 0; s < DrumPattern::kSteps; ++s)
                        anyKick = anyKick || p.stepFires (0, s, 0);
                    expect (anyKick, styleName (style) + "/" + densityName (d) + " must have a kick");
                }
            }
        }

        beginTest ("determinism — same seed replays bit-identically; backbone is seed-independent");
        {
            DrumPattern a, b;
            for (auto* p : { &a, &b })
            {
                p->loadStyle  (DrumPattern::Style::Rock);
                p->setDensity (DrumPattern::Density::Full);   // ornaments in range
            }

            // Identical seeds ⇒ identical fire/velocity sequences over 8 bars.
            for (uint32_t bar = 0; bar < 8; ++bar)
                for (int v = 0; v < kVoices; ++v)
                    for (int s = 0; s < DrumPattern::kSteps; ++s)
                    {
                        expect (a.stepFires (v, s, bar) == b.stepFires (v, s, bar),
                                "same seed must fire identically");
                        expectEquals (a.stepVelocity (v, s, bar), b.stepVelocity (v, s, bar),
                                      "same seed must produce identical velocities");
                    }

            // A different seed must keep the probability-255 backbone identical...
            b.setSeed (0x1234ABCDu);
            bool ornamentsDiffer = false;
            for (uint32_t bar = 0; bar < 8; ++bar)
                for (int v = 0; v < kVoices; ++v)
                    for (int s = 0; s < DrumPattern::kSteps; ++s)
                    {
                        const auto& cell = a.getActiveTable().cells[v][s];
                        if (cell.probability == 255)
                        {
                            expect (a.stepFires (v, s, bar) == b.stepFires (v, s, bar),
                                    "deterministic backbone must not depend on the seed");
                            if (cell.velRandRange == 0)
                                expectEquals (a.stepVelocity (v, s, bar), b.stepVelocity (v, s, bar),
                                              "un-humanized backbone velocity must not depend on the seed");
                        }
                        else if (a.stepFires (v, s, bar) != b.stepFires (v, s, bar))
                        {
                            ornamentsDiffer = true;   // ...while the ornament layer varies.
                        }
                    }
            expect (ornamentsDiffer,
                    "different seeds must produce a different ornament pattern over 8 bars");
        }

        beginTest ("velocity model — intensity re-voices monotonically; ghosts stay ghosts");
        {
            DrumPattern p;
            p.loadStyle (DrumPattern::Style::Rock);
            p.setComplexity (1.0f);

            // Kick on the downbeat: monotone in intensity, spanning velLow..velHigh.
            const auto& kickCell = p.getActiveTable().cells[0][0];
            float previous = -1.0f;
            for (int i10 = 0; i10 <= 10; ++i10)
            {
                p.setIntensity ((float) i10 / 10.0f);
                const float vel = p.stepVelocity (0, 0, 0);
                expect (vel >= previous, "velocity must not decrease with intensity");
                expect (vel >= (float) kickCell.velLow  / 127.0f - 1.0e-4f
                        && vel <= (float) kickCell.velHigh / 127.0f + 1.0e-4f,
                        "backbone velocity must stay inside the authored span");
                previous = vel;
            }

            p.setIntensity (0.0f);
            expectWithinAbsoluteError (p.stepVelocity (0, 0, 0),
                                       (float) kickCell.velLow / 127.0f, 1.0e-4f);
            p.setIntensity (1.0f);
            expectWithinAbsoluteError (p.stepVelocity (0, 0, 0),
                                       (float) kickCell.velHigh / 127.0f, 1.0e-4f);

            // A ghost cell is authored with a low, flat span: even at full
            // intensity it must stay far below the backbone (re-voicing, not
            // scaling). Rock's snare ghost sits at step 15.
            const auto& ghostCell = p.getActiveTable().cells[1][15];
            expect (ghostCell.velHigh > 0 && ghostCell.probability < 255
                    && (ghostCell.flags & StepCell::kFlagGhost) != 0,
                    "Rock must author a snare ghost at step 15");
            for (uint32_t bar = 0; bar < 8; ++bar)
            {
                const float ghostVel = p.stepVelocity (1, 15, bar);
                expect (ghostVel < 0.35f, "a ghost must stay a ghost at full intensity");
                expect (ghostVel < 0.5f * p.stepVelocity (0, 0, bar),
                        "ghost must stay well below the backbone level");
            }
        }

        beginTest ("Follow accent — never changes which cells fire (the hard constraint)");
        {
            for (auto style : allStyles)
                for (float c : { 0.2f, 0.55f, 1.0f })
                {
                    DrumPattern p;
                    p.loadStyle (style);
                    p.setComplexity (c);

                    bool before[kVoices][DrumPattern::kSteps][8];
                    for (uint32_t bar = 0; bar < 8; ++bar)
                        for (int v = 0; v < kVoices; ++v)
                            for (int s = 0; s < DrumPattern::kSteps; ++s)
                                before[v][s][bar] = p.stepFires (v, s, bar);

                    p.setFollowAccent (1.0f, 0.4f);   // maximum Follow accent

                    for (uint32_t bar = 0; bar < 8; ++bar)
                        for (int v = 0; v < kVoices; ++v)
                            for (int s = 0; s < DrumPattern::kSteps; ++s)
                                expect (p.stepFires (v, s, bar) == before[v][s][bar],
                                        styleName (style) + " complexity " + juce::String (c)
                                        + ": Follow accent must never change stepFires (voice "
                                        + juce::String (v) + ", step " + juce::String (s)
                                        + ", bar " + juce::String ((int) bar) + ")");
                }
        }

        beginTest ("Follow accent — neutral value and zero depth are exact no-ops");
        {
            DrumPattern p;
            p.loadStyle (DrumPattern::Style::Rock);
            p.setComplexity (1.0f);
            p.setIntensity (0.6f);

            float baseline[kVoices][DrumPattern::kSteps];
            for (int v = 0; v < kVoices; ++v)
                for (int s = 0; s < DrumPattern::kSteps; ++s)
                    baseline[v][s] = p.stepVelocity (v, s, 3);

            p.setFollowAccent (DrumPattern::kAccentNeutral, 0.4f);   // neutral accent, nonzero depth
            for (int v = 0; v < kVoices; ++v)
                for (int s = 0; s < DrumPattern::kSteps; ++s)
                    expectEquals (p.stepVelocity (v, s, 3), baseline[v][s],
                                  "accent == kAccentNeutral must be an exact no-op regardless of depth");

            p.setFollowAccent (1.0f, 0.0f);   // max accent value, zero depth
            for (int v = 0; v < kVoices; ++v)
                for (int s = 0; s < DrumPattern::kSteps; ++s)
                    expectEquals (p.stepVelocity (v, s, 3), baseline[v][s],
                                  "depth == 0 must be an exact no-op regardless of accent value");
        }

        beginTest ("Follow accent — bipolar around neutral, weighted by cell flags");
        {
            DrumPattern p;
            p.loadStyle (DrumPattern::Style::Rock);
            p.setComplexity (1.0f);
            p.setIntensity (0.5f);

            const int vKick = 0, vSnare = 1, vHiHat = 2;
            const uint32_t bar = 2;

            const auto& kickCell    = p.getActiveTable().cells[vKick][0];
            const auto& ghostCell   = p.getActiveTable().cells[vSnare][15];
            const auto& regularCell = p.getActiveTable().cells[vHiHat][4];
            expect ((kickCell.flags & StepCell::kFlagAccent) != 0, "Rock kick[0] must be accent-flagged");
            expect ((ghostCell.flags & StepCell::kFlagGhost) != 0, "Rock snare[15] must be ghost-flagged");
            expect ((regularCell.flags & (StepCell::kFlagAccent | StepCell::kFlagGhost)) == 0,
                    "Rock hihat[4] must carry neither flag");

            // Baselines at the default (no Follow) state.
            const float kickBase    = p.stepVelocity (vKick,  0,  bar);
            const float ghostBase   = p.stepVelocity (vSnare, 15, bar);
            const float regularBase = p.stepVelocity (vHiHat, 4,  bar);

            p.setFollowAccent (1.0f, 0.4f);   // a guide accent just landed
            const float kickHigh    = p.stepVelocity (vKick,  0,  bar);
            const float ghostHigh   = p.stepVelocity (vSnare, 15, bar);
            const float regularHigh = p.stepVelocity (vHiHat, 4,  bar);

            p.setFollowAccent (0.0f, 0.4f);   // between accents (the quietest reading)
            const float kickLow    = p.stepVelocity (vKick,  0,  bar);
            const float ghostLow   = p.stepVelocity (vSnare, 15, bar);
            const float regularLow = p.stepVelocity (vHiHat, 4,  bar);

            expectGreaterThan (kickHigh, kickBase, "a guide accent must raise an accent-flagged cell above baseline");
            expectLessThan    (kickLow,  kickBase, "accent == 0 must lower an accent-flagged cell below baseline");

            expectEquals (ghostHigh, ghostBase, "a ghost cell must be immune to the Follow accent term");
            expectEquals (ghostLow,  ghostBase, "a ghost cell must be immune to the Follow accent term (low accent too)");

            expectGreaterThan (regularHigh, regularBase, "a regular cell must still lean up on a guide accent");

            // Compare the *downward* swing (base - low), not the upward one: Rock's
            // kick[0] is authored velLow=98/velHigh=127, so at this intensity its
            // baseline already sits close to the [0,1] ceiling — the upward term
            // gets clamped away (exactly the near-saturation case the bipolar
            // design exists for; see DrumPattern's setFollowAccent comment), which
            // would make this comparison flaky in that direction. Downward from a
            // comfortable baseline hits neither clamp, so it cleanly isolates the
            // weight ordering the accent term is meant to test.
            expectLessThan (regularBase - regularLow, kickBase - kickLow,
                            "a regular cell's downward swing must be smaller than an accent-flagged cell's (weight 0.4 vs 1.0)");
        }

        beginTest ("Follow accent — final velocity always stays clamped to [0, 1]");
        {
            DrumPattern p;
            p.loadStyle (DrumPattern::Style::Rock);
            p.setComplexity (1.0f);

            // Near the top of the authored span + maximum positive accent must not exceed 1.0.
            p.setIntensity (1.0f);
            p.setFollowAccent (1.0f, 0.4f);
            expectLessOrEqual (p.stepVelocity (0, 0, 0), 1.0f);

            // Near the bottom of the span + maximum negative accent must not go below 0.0.
            p.setIntensity (0.0f);
            p.setFollowAccent (0.0f, 0.4f);
            expectGreaterOrEqual (p.stepVelocity (0, 0, 0), 0.0f);
        }

        beginTest ("seeded hash — uniform range, reproducible, decorrelated streams");
        {
            bool inRange = true, reproducible = true;
            for (uint32_t n = 0; n < 200; ++n)
            {
                const uint32_t seed = 0x9E3779B9u * (n + 1);
                const float h = groove::hash01 (seed, n, n % 6, n % 16, 0);
                inRange      = inRange && (h >= 0.0f && h < 1.0f);
                reproducible = reproducible
                            && juce::exactlyEqual (h, groove::hash01 (seed, n, n % 6, n % 16, 0));
            }
            expect (inRange,      "hash01 must stay in [0, 1)");
            expect (reproducible, "hash01 must be a pure function of its inputs");

            // Streams and bars must decorrelate (spot checks — collisions in a
            // 24-bit draw are possible in principle but not for these inputs).
            expect (! juce::exactlyEqual (groove::hash01 (1u, 0u, 0u, 0u, 0),
                                          groove::hash01 (1u, 0u, 0u, 0u, 1)),
                    "streams must decorrelate");
            expect (! juce::exactlyEqual (groove::hash01 (1u, 0u, 0u, 0u, 0),
                                          groove::hash01 (1u, 1u, 0u, 0u, 0)),
                    "bars must decorrelate");
            expect (! juce::exactlyEqual (groove::hash01 (1u, 0u, 0u, 0u, 0),
                                          groove::hash01 (2u, 0u, 0u, 0u, 0)),
                    "seeds must decorrelate");
            expect (groove::hashBipolar (1u, 0u, 0u, 0u, 1) >= -1.0f
                    && groove::hashBipolar (1u, 0u, 0u, 0u, 1) < 1.0f,
                    "hashBipolar must stay in [-1, 1)");
        }
    }

private:
    /** Voice bitmask of the deterministic (probability-255) hits at `step` for
        the pattern's current complexity — the legacy-mask view of the table. */
    static uint8_t backboneMask (const DrumPattern& p, int step)
    {
        uint8_t m = 0;
        for (int v = 0; v < kVoices; ++v)
            if (p.getActiveTable().cells[v][step].probability == 255
                && p.stepFires (v, step, 0))
                m = (uint8_t) (m | (1u << v));
        return m;
    }

    static int countFires (const DrumPattern& p, uint32_t barIndex)
    {
        int n = 0;
        for (int v = 0; v < kVoices; ++v)
            for (int s = 0; s < DrumPattern::kSteps; ++s)
                n += p.stepFires (v, s, barIndex) ? 1 : 0;
        return n;
    }

    static juce::String styleName (DrumPattern::Style s)
    {
        switch (s)
        {
            case DrumPattern::Style::Rock:        return "Rock";
            case DrumPattern::Style::Jazz:        return "Jazz";
            case DrumPattern::Style::Electronic:  break;
        }
        return "Electronic";
    }

    static juce::String densityName (DrumPattern::Density d)
    {
        switch (d)
        {
            case DrumPattern::Density::Sparse:    return "Sparse";
            case DrumPattern::Density::Medium:    return "Medium";
            case DrumPattern::Density::Full:      break;
        }
        return "Full";
    }
};

static DrumPatternTest drumPatternTest;
