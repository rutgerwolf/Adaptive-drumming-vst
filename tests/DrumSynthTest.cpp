#include <JuceHeader.h>
#include "drummer/DrumSynth.h"
#include "drummer/DrumPattern.h"

#include <cmath>

/**
 * DrumSynth tests — the synth must make finite, audible sound when a step
 * triggers, stay silent when nothing triggers, and keep a voice ringing across
 * block boundaries.
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
            synth.processBlock (buf, 512, sr, pattern, bpm, 0);   // step 0 → kick at offset 0

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
            synth.processBlock (buf, 512, sr, pattern, bpm, 100);  // mid-step, no boundary

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
                synth.processBlock (block, blockSize, sr, pattern, bpm, playhead);

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
    }
};

static DrumSynthTest drumSynthTest;
