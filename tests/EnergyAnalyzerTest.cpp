#include <JuceHeader.h>
#include "drummer/EnergyAnalyzer.h"

/**
 * Tests for EnergyAnalyzer's guide-track analysis: the fast "energy" envelope
 * (unchanged), the slower "macro" envelope (phrase-level contour), and the
 * "accent" signal derived from how far fast currently pokes above macro.
 *
 * macro/accent feed Follow's modulation of the groove (see PluginProcessor and
 * DrumPattern::setFollowAccent) — never the 3-step density this analyzer used
 * to expose, which is retired along with these tests.
 */
class EnergyAnalyzerTest : public juce::UnitTest
{
public:
    EnergyAnalyzerTest() : juce::UnitTest ("EnergyAnalyzer", "drummer") {}

    void runTest() override
    {
        beginTest ("steady signal settles: low accent, macro tracks energy");
        {
            EnergyAnalyzer a;
            a.prepare (48000.0, 512);

            juce::AudioBuffer<float> buf (1, 512);
            const float amp = juce::Decibels::decibelsToGain (-20.0f);   // well inside the mapping window
            for (int i = 0; i < 512; ++i)
                buf.setSample (0, i, amp);

            for (int i = 0; i < 300; ++i)   // ~3.2 s at 512/48000 — plenty to fully settle both envelopes
                a.processBlock (buf, 512);

            expectLessThan (a.getAccent(), 0.1f, "a steady signal must not read as a constant accent");
            expectWithinAbsoluteError (a.getMacroEnergy(), a.getEnergy(), 0.05f,
                                      "macro must have caught up to the fast envelope once settled");
        }

        beginTest ("sudden louder guide spikes the accent, then hands off to macro");
        {
            EnergyAnalyzer a;
            a.prepare (48000.0, 512);

            juce::AudioBuffer<float> quiet (1, 512), loud (1, 512);
            const float quietAmp = juce::Decibels::decibelsToGain (-40.0f);
            const float loudAmp  = juce::Decibels::decibelsToGain (-16.0f);
            for (int i = 0; i < 512; ++i)
            {
                quiet.setSample (0, i, quietAmp);
                loud.setSample  (0, i, loudAmp);
            }

            for (int i = 0; i < 300; ++i)          // settle at the quiet level first
                a.processBlock (quiet, 512);
            expectLessThan (a.getAccent(), 0.1f, "settled quiet level must not read as an accent");

            const int blocksPerSec = static_cast<int> (48000.0 / 512.0 + 0.5);   // ~94

            float peakAccent = 0.0f;
            for (int i = 0; i < blocksPerSec / 2; ++i)   // ~500 ms into the loud section
            {
                a.processBlock (loud, 512);
                peakAccent = juce::jmax (peakAccent, a.getAccent());
            }
            expectGreaterThan (peakAccent, 0.35f, "a sudden louder guide must spike the accent signal");

            for (int i = 0; i < blocksPerSec * 2; ++i)   // 2 more seconds — well past the accent's decay
                a.processBlock (loud, 512);
            expectLessThan (a.getAccent(), 0.15f,
                            "accent must hand off to the macro envelope, not stay elevated");
            expectGreaterThan (a.getMacroEnergy(), 0.5f,
                               "macro must have caught up to the sustained louder level");
        }

        beginTest ("silence decays energy, macro and accent back down");
        {
            EnergyAnalyzer a;
            a.prepare (48000.0, 512);

            juce::AudioBuffer<float> loud (1, 512);
            const float loudAmp = juce::Decibels::decibelsToGain (-16.0f);
            for (int i = 0; i < 512; ++i)
                loud.setSample (0, i, loudAmp);

            for (int i = 0; i < 300; ++i)
                a.processBlock (loud, 512);

            for (int i = 0; i < 300; ++i)
                a.processSilence (512);

            expectLessThan (a.getEnergy(),      0.05f, "energy must decay to ~silence");
            expectLessThan (a.getMacroEnergy(), 0.05f, "macro must decay to ~silence too");
            expectLessThan (a.getAccent(),      0.05f, "accent must settle to ~0 once both envelopes agree at silence");
        }
    }
};

static EnergyAnalyzerTest energyAnalyzerTest;
