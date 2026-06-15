#include <JuceHeader.h>
#include "drummer/AdaptiveDrummer.h"

/**
 * Tests for B3 — mapping the host ppq position to an in-pattern sample offset
 * so the drummer locks to the DAW bar line.
 */
class AdaptiveDrummerTest : public juce::UnitTest
{
public:
    AdaptiveDrummerTest() : juce::UnitTest ("AdaptiveDrummer", "drummer") {}

    void runTest() override
    {
        beginTest ("ppqToPlayhead — bar-relative mapping (4/4)");
        {
            const int len = 96000;  // one bar, in samples
            expectEquals (AdaptiveDrummer::ppqToPlayhead (0.0, len), 0);
            expectEquals (AdaptiveDrummer::ppqToPlayhead (4.0, len), 0);          // exactly 1 bar → wraps
            expectEquals (AdaptiveDrummer::ppqToPlayhead (8.0, len), 0);          // 2 bars
            expectEquals (AdaptiveDrummer::ppqToPlayhead (2.0, len), len / 2);    // half bar
            expectEquals (AdaptiveDrummer::ppqToPlayhead (1.0, len), len / 4);    // 1 beat
            expectEquals (AdaptiveDrummer::ppqToPlayhead (4.5, len), len / 8);    // half-beat into bar 2
        }

        beginTest ("ppqToPlayhead — result always in [0, patternLen)");
        {
            const int len = 88200;
            for (double ppq : { -3.25, -0.001, 0.0, 3.999, 5.5, 123.456 })
            {
                const int s = AdaptiveDrummer::ppqToPlayhead (ppq, len);
                expect (s >= 0 && s < len, "out of range for ppq " + juce::String (ppq));
            }
        }

        beginTest ("ppqToPlayhead — negative ppq wraps forward");
        {
            const int len = 80000;
            // one beat before bar 1 = three-quarters through the previous bar
            expectEquals (AdaptiveDrummer::ppqToPlayhead (-1.0, len), (len * 3) / 4);
        }

        beginTest ("ppqToPlayhead — degenerate inputs are safe");
        {
            expectEquals (AdaptiveDrummer::ppqToPlayhead (1.0, 0), 0);
            expectEquals (AdaptiveDrummer::ppqToPlayhead (1.0, 88200, 0.0), 0);
        }
    }
};

static AdaptiveDrummerTest adaptiveDrummerTest;
