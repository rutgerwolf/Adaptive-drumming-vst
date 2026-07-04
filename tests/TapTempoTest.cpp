#include <JuceHeader.h>
#include "TapTempo.h"

/**
 * Pure-math tests for TapTempo::computeBpm() (the editor's TAP button).
 * No GUI/editor instantiation needed — this codebase's tests never call
 * createEditor() (see PluginEditor.h's class comment); computeBpm() has no
 * GUI dependency, only juce::Array, so it's testable directly.
 */
class TapTempoTest : public juce::UnitTest
{
public:
    TapTempoTest() : juce::UnitTest ("TapTempo", "editor") {}

    void runTest() override
    {
        beginTest ("fewer than 2 taps produce no result");
        {
            juce::Array<double> none;
            expectEquals (TapTempo::computeBpm (none), 0.0);

            juce::Array<double> one { 1000.0 };
            expectEquals (TapTempo::computeBpm (one), 0.0);
        }

        beginTest ("two taps exactly 500ms apart => 120 BPM");
        {
            juce::Array<double> taps { 1000.0, 1500.0 };
            expectWithinAbsoluteError (TapTempo::computeBpm (taps), 120.0, 1.0e-9);
        }

        beginTest ("many taps at a steady 400ms interval => 150 BPM");
        {
            juce::Array<double> taps;
            for (int i = 0; i < 8; ++i)
                taps.add ((double) i * 400.0);
            expectWithinAbsoluteError (TapTempo::computeBpm (taps), 150.0, 1.0e-6);
        }

        beginTest ("averages across the whole span — robust to one jittery interval");
        {
            // Intervals 500/550/450ms: a naive "last interval only" estimate
            // would read the 450ms tail as 133.3 BPM; the whole-span average
            // must land close to the true 120 BPM instead.
            juce::Array<double> taps { 0.0, 500.0, 1050.0, 1500.0 };
            expectWithinAbsoluteError (TapTempo::computeBpm (taps), 120.0, 5.0);
        }

        beginTest ("duplicate or non-increasing timestamps produce no result, not a divide-by-zero");
        {
            juce::Array<double> duplicate { 1000.0, 1000.0 };
            expectEquals (TapTempo::computeBpm (duplicate), 0.0);

            juce::Array<double> reversed { 1000.0, 500.0 };   // negative span
            expectEquals (TapTempo::computeBpm (reversed), 0.0);
        }
    }
};

static TapTempoTest tapTempoTest;
