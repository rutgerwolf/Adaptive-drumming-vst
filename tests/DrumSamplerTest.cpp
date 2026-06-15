#include <JuceHeader.h>
#include "drummer/DrumSampler.h"
#include "drummer/DrumPattern.h"

#include <cmath>

/**
 * DrumSampler tests.
 *
 * The headline test is the B1 regression: a hit whose sample spans several
 * process blocks must render as one continuous, gap-free burst. Before the
 * triggerOffset reset, every block after the first re-skipped `triggerOffset`
 * samples, leaving a periodic silent gap (an audible click). This test fails
 * against that bug and passes once it is fixed.
 */
class DrumSamplerTest : public juce::UnitTest
{
public:
    DrumSamplerTest() : juce::UnitTest ("DrumSampler", "drummer") {}

    void runTest() override
    {
        beginTest ("B1 — sustained sample renders gap-free across blocks");
        {
            const double sr        = 48000.0;
            const int    sampleLen = 2000;   // longer than several blocks

            auto kitRoot = makeTempKitWithKick (sampleLen, sr);

            DrumSampler sampler;
            sampler.prepare (sr);
            expect (sampler.loadSamples (kitRoot), "temp kit should load");
            expect (sampler.areSamplesLoaded());

            // Electronic / Sparse fires the kick (and only the kick) on step 0.
            DrumPattern pattern;
            pattern.loadStyle  (DrumPattern::Style::Electronic);
            pattern.setDensity (DrumPattern::Density::Sparse);

            const double bpm        = 60.0;
            const int    patternLen = pattern.getLengthInSamples (bpm, sr); // 192000
            const int    stepLen    = patternLen / DrumPattern::kSteps;     // 12000

            // Begin 10 samples before bar 1 so step 0 fires at in-block offset 10.
            const int triggerOffset = 10;
            int       playhead      = patternLen - triggerOffset;

            const int blockSize = 512;
            const int numBlocks = 6;                 // 3072 samples captured
            const int total     = numBlocks * blockSize;

            // Guard the premise: only a single trigger inside the captured window.
            expectGreaterThan (stepLen, total);

            juce::AudioBuffer<float> captured (1, total);
            captured.clear();

            juce::AudioBuffer<float> block (1, blockSize);
            for (int b = 0; b < numBlocks; ++b)
            {
                block.clear();
                sampler.processBlock (block, blockSize, sr, pattern, bpm, playhead);
                captured.copyFrom (0, b * blockSize, block, 0, 0, blockSize);
                playhead = (playhead + blockSize) % patternLen;
            }

            // Expect: silence in [0, 10), the DC hit in [10, 2010), silence after.
            const float* d  = captured.getReadPointer (0);
            bool  contiguous = true;
            int   firstBad   = -1;
            for (int i = 0; i < total; ++i)
            {
                const bool  inHit    = (i >= triggerOffset && i < triggerOffset + sampleLen);
                const float expected = inHit ? 1.0f : 0.0f;
                if (std::abs (d[i] - expected) > 1.0e-3f)
                {
                    contiguous = false;
                    firstBad   = i;
                    break;
                }
            }

            if (! contiguous)
                logMessage ("First gap/mismatch at sample " + juce::String (firstBad)
                            + " (value " + juce::String (d[firstBad]) + ")");

            expect (contiguous, "hit must be one contiguous burst with no interior gaps");

            kitRoot.deleteRecursively();
        }

        beginTest ("missing kit → nothing loaded, output stays silent");
        {
            DrumSampler sampler;
            sampler.prepare (44100.0);

            const auto missing = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                     .getChildFile ("adk_no_such_kit_dir_42");
            expect (! sampler.loadSamples (missing), "non-existent kit must not load");
            expect (! sampler.areSamplesLoaded());

            DrumPattern pattern;             // default Rock / Medium
            juce::AudioBuffer<float> block (2, 256);
            block.clear();
            sampler.processBlock (block, 256, 44100.0, pattern, 120.0, 0);

            expectEquals (block.getMagnitude (0, 256), 0.0f);
        }
    }

private:
    // Creates <temp>/adk_test_kit_XXXX/kick/kick.wav containing `numSamples`
    // of DC = 1.0 (mono). Returns the kit root directory.
    juce::File makeTempKitWithKick (int numSamples, double sr)
    {
        auto root = juce::File::createTempFile ("adk_test_kit");
        root.deleteFile();
        root.createDirectory();

        auto kickDir = root.getChildFile ("kick");
        kickDir.createDirectory();

        auto wav = kickDir.getChildFile ("kick.wav");
        wav.deleteFile();

        juce::WavAudioFormat fmt;
        if (auto* os = wav.createOutputStream().release())
        {
            std::unique_ptr<juce::AudioFormatWriter> writer (
                fmt.createWriterFor (os, sr, 1, 16, {}, 0));

            if (writer != nullptr)
            {
                juce::AudioBuffer<float> buf (1, numSamples);
                for (int i = 0; i < numSamples; ++i)
                    buf.setSample (0, i, 1.0f);
                writer->writeFromAudioSampleBuffer (buf, 0, numSamples);
            }
            else
            {
                delete os;   // writer takes ownership only on success
            }
        }

        return root;
    }
};

static DrumSamplerTest drumSamplerTest;
