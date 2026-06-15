#include <JuceHeader.h>
#include "drummer/EnergyAnalyzer.h"
#include "drummer/DrumPattern.h"

/**
 * Tests for the adaptive energy→density mapping (Phase 3).
 *
 * Two layers: the pure hysteresis function (deterministic, edge cases) and the
 * block-rate envelope behaviour (silence settles low, loud settles high).
 */
class EnergyAnalyzerTest : public juce::UnitTest
{
public:
    EnergyAnalyzerTest() : juce::UnitTest ("EnergyAnalyzer", "drummer") {}

    void runTest() override
    {
        using D = DrumPattern::Density;

        beginTest ("hysteresis dead-band");
        {
            // Rising out of Sparse needs to clear the up-threshold (0.30).
            expect (EnergyAnalyzer::nextDensity (D::Sparse, 0.25f) == D::Sparse);
            expect (EnergyAnalyzer::nextDensity (D::Sparse, 0.40f) == D::Medium);

            // Inside the band Medium is sticky in both directions.
            expect (EnergyAnalyzer::nextDensity (D::Medium, 0.25f) == D::Medium);
            expect (EnergyAnalyzer::nextDensity (D::Medium, 0.20f) == D::Sparse);
            expect (EnergyAnalyzer::nextDensity (D::Medium, 0.70f) == D::Full);

            // Falling out of Full needs to drop below 0.58, not just below 0.66.
            expect (EnergyAnalyzer::nextDensity (D::Full, 0.60f) == D::Full);
            expect (EnergyAnalyzer::nextDensity (D::Full, 0.50f) == D::Medium);
            expect (EnergyAnalyzer::nextDensity (D::Full, 0.10f) == D::Sparse);
        }

        beginTest ("silence settles to Sparse, energy ~0");
        {
            EnergyAnalyzer a;
            a.prepare (48000.0, 512);
            for (int i = 0; i < 200; ++i)
                a.processSilence (512);

            expect (a.getEnergy() < 0.05f);
            expect (a.getDensity() == D::Sparse);
        }

        beginTest ("loud sustained guide drives density to Full");
        {
            EnergyAnalyzer a;
            a.prepare (48000.0, 512);

            juce::AudioBuffer<float> buf (2, 512);   // 0.5 const ≈ -6 dBFS RMS
            for (int ch = 0; ch < buf.getNumChannels(); ++ch)
                for (int i = 0; i < 512; ++i)
                    buf.setSample (ch, i, 0.5f);

            for (int i = 0; i < 200; ++i)
                a.processBlock (buf, 512);

            expectGreaterThan (a.getEnergy(), 0.66f);
            expect (a.getDensity() == D::Full);
        }

        beginTest ("moderate guide settles to Medium");
        {
            EnergyAnalyzer a;
            a.prepare (48000.0, 512);

            // Aim the mapping at ~0.40 energy: dB = -48 + 0.4*36 = -33.6 dBFS.
            const float amp = juce::Decibels::decibelsToGain (-33.6f);
            juce::AudioBuffer<float> buf (1, 512);
            for (int i = 0; i < 512; ++i)
                buf.setSample (0, i, amp);

            for (int i = 0; i < 300; ++i)
                a.processBlock (buf, 512);

            expectWithinAbsoluteError (a.getEnergy(), 0.40f, 0.06f);
            expect (a.getDensity() == D::Medium);
        }
    }
};

static EnergyAnalyzerTest energyAnalyzerTest;
