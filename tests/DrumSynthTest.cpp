#include <JuceHeader.h>
#include "drummer/DrumSynth.h"
#include "drummer/DrumPattern.h"

#include <cmath>

/**
 * DrumSynth tests — the synth must make finite, audible sound when a step
 * triggers, stay silent when nothing triggers, keep a voice ringing across
 * block boundaries, and scale a hit's level with the pattern's per-hit
 * velocity (the intensity axis).
 */
class DrumSynthTest : public juce::UnitTest
{
public:
    DrumSynthTest() : juce::UnitTest ("DrumSynth", "drummer") {}

    void runTest() override
    {
        const double sr  = 48000.0;
        const double bpm = 60.0;

        DrumPattern pattern;
        pattern.loadStyle  (DrumPattern::Style::Electronic);
        pattern.setDensity (DrumPattern::Density::Sparse);   // kick on step 0
        const int patternLen = pattern.getLengthInSamples (bpm, sr);

        beginTest ("triggered hit is finite, bounded and audible");
        {
            DrumSynth synth;
            synth.prepare (sr);

            juce::AudioBuffer<float> buf (2, 512);
            buf.clear();
            synth.processBlock (buf, 512, sr, pattern, bpm, 0, 0);   // step 0 → kick at offset 0

            expectGreaterThan (buf.getMagnitude (0, 512), 0.01f);

            bool finite = true;
            for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            {
                const float* d = buf.getReadPointer (ch);
                for (int i = 0; i < 512; ++i)
                    if (! std::isfinite (d[i]) || std::abs (d[i]) > 4.0f)
                        finite = false;
            }
            expect (finite, "output must be finite and within a sane range");

            // The synth sums mono into every channel.
            expectEquals (buf.getReadPointer (1)[100], buf.getReadPointer (0)[100]);
        }

        beginTest ("no trigger in the block → silence");
        {
            DrumSynth synth;
            synth.prepare (sr);

            juce::AudioBuffer<float> buf (1, 512);
            buf.clear();
            synth.processBlock (buf, 512, sr, pattern, bpm, 100, 0);  // mid-step, no boundary

            expectEquals (buf.getMagnitude (0, 512), 0.0f);
        }

        beginTest ("voice keeps ringing across blocks");
        {
            DrumSynth synth;
            synth.prepare (sr);

            const int blockSize = 512;
            const int numBlocks = 8;                 // 4096 samples < kick decay
            int       playhead  = patternLen - 10;   // kick fires at in-block offset 10

            juce::AudioBuffer<float> block (1, blockSize);
            bool everyBlockRings = true;

            for (int b = 0; b < numBlocks; ++b)
            {
                block.clear();
                synth.processBlock (block, blockSize, sr, pattern, bpm, playhead, 0);

                double sumSq = 0.0;
                const float* d = block.getReadPointer (0);
                for (int i = 0; i < blockSize; ++i)
                    sumSq += static_cast<double> (d[i]) * d[i];
                const double rms = std::sqrt (sumSq / blockSize);

                if (rms < 1.0e-4)
                {
                    everyBlockRings = false;
                    logMessage ("block " + juce::String (b) + " rms=" + juce::String (rms));
                }
                playhead = (playhead + blockSize) % patternLen;
            }

            expect (everyBlockRings, "the kick should sustain across block boundaries");
        }

        beginTest ("velocity is live — higher intensity produces a measurably louder hit");
        {
            // Same kick, same step, two intensities: the pattern's velocity
            // (lerp velLow→velHigh) must audibly scale the synthesized hit.
            DrumPattern quiet, loud;
            for (auto* p : { &quiet, &loud })
            {
                p->loadStyle (DrumPattern::Style::Electronic);
                p->setComplexity (0.2f);   // kick backbone only
            }
            quiet.setIntensity (0.2f);
            loud.setIntensity  (0.9f);

            const float velQuiet = quiet.stepVelocity (0, 0, 0);
            const float velLoud  = loud.stepVelocity  (0, 0, 0);
            expectGreaterThan (velLoud, velQuiet, "pattern velocity must rise with intensity");

            auto renderPeak = [&] (const DrumPattern& p)
            {
                DrumSynth synth;
                synth.prepare (sr);
                juce::AudioBuffer<float> buf (1, 512);
                buf.clear();
                synth.processBlock (buf, 512, sr, p, bpm, 0, 0);   // kick at offset 0
                return buf.getMagnitude (0, 512);
            };

            const float peakQuiet = renderPeak (quiet);
            const float peakLoud  = renderPeak (loud);

            logMessage ("velocity live: vel " + juce::String (velQuiet, 3) + " → peak "
                        + juce::String (peakQuiet, 4) + "; vel " + juce::String (velLoud, 3)
                        + " → peak " + juce::String (peakLoud, 4));

            expectGreaterThan (peakQuiet, 0.0f, "quiet hit must still sound");
            expectGreaterThan (peakLoud, peakQuiet * 1.15f,
                               "intensity 0.9 must be measurably louder than 0.2");
        }
    }
};

static DrumSynthTest drumSynthTest;
