#include <JuceHeader.h>
#include "drummer/DrumPattern.h"

/**
 * Regression net for the pattern/timing maths — the part most likely to break
 * silently when patterns or tempo handling are edited.
 */
class DrumPatternTest : public juce::UnitTest
{
public:
    DrumPatternTest() : juce::UnitTest ("DrumPattern", "drummer") {}

    void runTest() override
    {
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

        beginTest ("getStep — out-of-range indices return 0");
        {
            DrumPattern p;
            expectEquals ((int) p.getStep (-1), 0);
            expectEquals ((int) p.getStep (DrumPattern::kSteps), 0);
            expectEquals ((int) p.getStep (10000), 0);
        }

        beginTest ("Rock / Medium — known masks");
        {
            DrumPattern p;
            p.loadStyle  (DrumPattern::Style::Rock);
            p.setDensity (DrumPattern::Density::Medium);
            expectEquals ((int) p.getStep (0), 0x05);  // Kick | HiHat on beat 1
            expectEquals ((int) p.getStep (2), 0x04);  // HiHat on the off-8th
            expectEquals ((int) p.getStep (4), 0x06);  // Snare | HiHat backbeat
        }

        beginTest ("density selects the active layer");
        {
            DrumPattern p;
            p.loadStyle (DrumPattern::Style::Rock);

            p.setDensity (DrumPattern::Density::Sparse);
            const int sparseHits = countHits (p);

            p.setDensity (DrumPattern::Density::Full);
            const int fullHits = countHits (p);

            expectGreaterThan (fullHits, sparseHits);
            // Full Rock opens with a crash
            expect ((p.getStep (0) & DrumPattern::Crash) != 0);
        }

        beginTest ("style selects a different pattern");
        {
            DrumPattern rock, jazz;
            rock.loadStyle (DrumPattern::Style::Rock);
            jazz.loadStyle (DrumPattern::Style::Jazz);
            rock.setDensity (DrumPattern::Density::Medium);
            jazz.setDensity (DrumPattern::Density::Medium);

            bool differ = false;
            for (int s = 0; s < DrumPattern::kSteps; ++s)
                if (rock.getStep (s) != jazz.getStep (s)) { differ = true; break; }

            expect (differ, "Rock and Jazz medium patterns must not be identical");
            expect ((jazz.getStep (0) & DrumPattern::Ride) != 0, "Jazz rides on beat 1");
        }
    }

private:
    static int countHits (const DrumPattern& p)
    {
        int n = 0;
        for (int s = 0; s < DrumPattern::kSteps; ++s)
            for (uint8_t m = p.getStep (s); m != 0; m >>= 1)
                n += (m & 1);
        return n;
    }
};

static DrumPatternTest drumPatternTest;
