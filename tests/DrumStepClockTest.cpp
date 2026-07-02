#include <JuceHeader.h>
#include "drummer/DrumStepClock.h"
#include "drummer/DrumPattern.h"

/**
 * Guard for the shared step clock (forEachStepTrigger in DrumStepClock.h).
 *
 * This is the timing-critical unit the host-sync rewrite (roadmap step 4) will
 * replace, so it is pinned first. The test drives the clock free-running over
 * exactly one bar, across a sweep of block sizes, and reconstructs which
 * (step, voice) fired from the running playhead. All assertions hold on the
 * current implementation:
 *   - every non-empty step fires its authored voices exactly once per bar;
 *   - no callback maps to a step index >= kSteps (no phantom 17th step) — checked
 *     via voice callbacks, not by inspecting the boundary scan;
 *   - firing is identical however the bar is split into blocks (block-edge safety).
 */
class DrumStepClockTest : public juce::UnitTest
{
public:
    DrumStepClockTest() : juce::UnitTest ("DrumStepClock", "drummer") {}

    struct Combo { DrumPattern::Style style; DrumPattern::Density density; const char* name; };

    void runTest() override
    {
        const Combo combos[] = {
            { DrumPattern::Style::Rock,       DrumPattern::Density::Medium, "Rock/Medium" },
            { DrumPattern::Style::Electronic, DrumPattern::Density::Full,   "Electronic/Full" },
            { DrumPattern::Style::Jazz,       DrumPattern::Density::Sparse, "Jazz/Sparse" },
        };
        // 130 BPM @ 44.1 kHz gives a non-integral bar (81415.38 samples) — the case
        // that stresses block-edge handling; 120 BPM @ 48 kHz is exactly divisible.
        const double tempos[][2] = { { 130.0, 44100.0 }, { 120.0, 48000.0 } };
        const int    blockSizes[] = { 1, 64, 480, 4096 };

        for (const auto& c : combos)
            for (const auto& t : tempos)
                for (int bs : blockSizes)
                    checkBar (c, t[0], t[1], bs);
    }

private:
    void checkBar (const Combo& c, double bpm, double sr, int blockSize)
    {
        beginTest (juce::String ("one free-running bar — ") + c.name
                   + " @ " + juce::String (bpm, 0) + "/" + juce::String (sr, 0)
                   + " block " + juce::String (blockSize));

        DrumPattern p;
        p.loadStyle  (c.style);
        p.setDensity (c.density);

        const int patternLen = p.getLengthInSamples (bpm, sr);
        const int stepLen     = patternLen / DrumPattern::kSteps;
        expectGreaterThan (patternLen, 0);
        expectGreaterThan (stepLen, 0);

        constexpr int kVoices = 6;
        int hitCount[DrumPattern::kSteps][kVoices] = {};
        int overrun = 0;   // callbacks mapping to a step index outside [0, kSteps)

        // Scan exactly one bar's worth of samples (positions 0..patternLen-1 once).
        int playhead = 0;
        int scanned  = 0;
        while (scanned < patternLen)
        {
            const int n = juce::jmin (blockSize, patternLen - scanned);
            forEachStepTrigger (p, playhead, n, bpm, sr,
                [&] (int voice, int offset)
                {
                    const long long absPos = (long long) playhead + offset;
                    const int pos  = (int) (absPos % patternLen);
                    const int step = pos / stepLen;
                    if (step >= 0 && step < DrumPattern::kSteps && voice >= 0 && voice < kVoices)
                        ++hitCount[step][voice];
                    else
                        ++overrun;
                });
            playhead += n;
            scanned  += n;
        }

        expectEquals (overrun, 0, "no callback may map past the 16th step");

        for (int s = 0; s < DrumPattern::kSteps; ++s)
        {
            uint8_t fired = 0;
            for (int v = 0; v < kVoices; ++v)
            {
                expect (hitCount[s][v] <= 1, "each (step, voice) fires at most once per bar");
                if (hitCount[s][v] > 0)
                    fired |= (uint8_t) (1u << v);
            }
            expectEquals ((int) fired, (int) p.getStep (s),
                          "reconstructed step voices must match the pattern");
        }
    }
};

static DrumStepClockTest drumStepClockTest;
