#include <JuceHeader.h>
#include "drummer/DrumSampler.h"
#include "drummer/DrumPattern.h"

#include <cmath>
#include <atomic>
#include <chrono>
#include <thread>

/**
 * DrumSampler tests.
 *
 * The headline test is the B1 regression: a hit whose sample spans several
 * process blocks must render as one continuous, gap-free burst. Before the
 * triggerOffset reset, every block after the first re-skipped `triggerOffset`
 * samples, leaving a periodic silent gap (an audible click). This test fails
 * against that bug and passes once it is fixed.
 *
 * The B2 test below is the polyphony-pool regression: retriggering the same
 * voice while a previous hit is still ringing must let both instances sound
 * concurrently instead of the newer hit stealing the single cursor.
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

            // The kick's per-hit velocity is now the slot gain, so the DC hit
            // renders at this level (deterministic: the backbone kick has no
            // velocity humanization).
            const float hitLevel = pattern.stepVelocity (0, 0, 0);
            expectGreaterThan (hitLevel, 0.0f);

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
                sampler.processBlock (block, blockSize, sr, pattern, bpm, playhead, 0);
                captured.copyFrom (0, b * blockSize, block, 0, 0, blockSize);
                playhead = (playhead + blockSize) % patternLen;
            }

            // Expect: silence in [0, 10), the DC hit at hitLevel in
            // [10, 2010), silence after — one contiguous burst, no gaps.
            const float* d  = captured.getReadPointer (0);
            bool  contiguous = true;
            int   firstBad   = -1;
            for (int i = 0; i < total; ++i)
            {
                const bool  inHit    = (i >= triggerOffset && i < triggerOffset + sampleLen);
                const float expected = inHit ? hitLevel : 0.0f;
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
            sampler.processBlock (block, 256, 44100.0, pattern, 120.0, 0, 0);

            expectEquals (block.getMagnitude (0, 256), 0.0f);
        }

        beginTest ("C1 — concurrent kit reload never races the audio callback");
        {
            const double sr = 48000.0;
            auto kitA = makeTempKitWithKick (500,  sr);
            auto kitB = makeTempKitWithKick (3000, sr);

            DrumSampler sampler;
            sampler.prepare (sr);
            expect (sampler.loadSamples (kitA), "kit A should load");

            DrumPattern pattern;
            pattern.loadStyle  (DrumPattern::Style::Rock);
            pattern.setDensity (DrumPattern::Density::Full);   // dense → many triggers
            const double bpm        = 140.0;
            const int    patternLen = pattern.getLengthInSamples (bpm, sr);

            std::atomic<bool> stop     { false };
            std::atomic<int>  loads    { 0 };
            std::atomic<bool> loaderOk { true };

            // Loader thread hammers loadSamples() (message-thread role): it builds a
            // new SampleSet and swaps it in under the SpinLock the audio thread only
            // try-locks. If allocation/free ever moved under the lock or onto the
            // audio thread, this would race and the output below would go non-finite.
            std::thread loader ([&]
            {
                bool useA = false;
                while (! stop.load (std::memory_order_relaxed))
                {
                    if (! sampler.loadSamples (useA ? kitA : kitB))
                        loaderOk.store (false, std::memory_order_relaxed);
                    loads.fetch_add (1, std::memory_order_relaxed);
                    useA = ! useA;
                }
            });

            const int blockSize = 256;
            juce::AudioBuffer<float> block (2, blockSize);
            bool allFinite = true;
            int  playhead  = 0;
            const auto tEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds (300);
            while (std::chrono::steady_clock::now() < tEnd)
            {
                block.clear();
                sampler.processBlock (block, blockSize, sr, pattern, bpm, playhead, 0);
                for (int ch = 0; ch < block.getNumChannels() && allFinite; ++ch)
                {
                    const float* d = block.getReadPointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                        if (! std::isfinite (d[i])) { allFinite = false; break; }
                }
                playhead = (playhead + blockSize) % patternLen;
            }
            stop.store (true, std::memory_order_relaxed);
            loader.join();

            expect (allFinite, "audio output must stay finite during concurrent reloads");
            expect (loaderOk.load(), "every concurrent kit load must succeed");
            expectGreaterThan (loads.load(), 1, "loader must have swapped kits repeatedly");
            expect (sampler.areSamplesLoaded(), "a kit remains loaded after the stress run");

            // A settled render still produces finite audio.
            block.clear();
            sampler.processBlock (block, blockSize, sr, pattern, bpm, 0, 0);
            expect (std::isfinite (block.getMagnitude (0, blockSize)));

            kitA.deleteRecursively();
            kitB.deleteRecursively();
        }

        beginTest ("B2 — overlapping same-voice hits ring concurrently instead of stealing the cursor");
        {
            const double sr        = 48000.0;
            const double bpm       = 120.0;
            const int    sampleLen = 50000;   // long enough to still be ringing at the next few kick hits
            const float  dcLevel   = 0.5f;    // two overlapping hits sum to 1.0, three to 1.5

            auto kitRoot = makeTempKitWithKick (sampleLen, sr, dcLevel);

            DrumSampler sampler;
            sampler.prepare (sr);
            expect (sampler.loadSamples (kitRoot), "temp kit should load");
            expect (sampler.areSamplesLoaded());

            // Electronic / Sparse fires the kick on steps 0, 4, 8, 12 (and the snare on
            // 4 & 12, but no snare sample is loaded, so only the kick sounds).
            DrumPattern pattern;
            pattern.loadStyle  (DrumPattern::Style::Electronic);
            pattern.setDensity (DrumPattern::Density::Sparse);

            const int patternLen = pattern.getLengthInSamples (bpm, sr);   // 96000
            const int stepLen    = patternLen / DrumPattern::kSteps;       // 6000
            expectEquals (patternLen, 96000);
            expectEquals (stepLen, 6000);

            // Kick hits land at 0 / 24000 / 48000 / 72000. Each ringing for 50000
            // samples means, e.g., the hits at 0 and 24000 are both still sounding
            // during [24000, 50000) — a direct, deterministic proof of polyphony.
            // The old single-cursor code would reset playPos on every retrigger and
            // could never produce more than one kick's worth of level.
            //
            // The four-on-the-floor kick is authored at a uniform velocity, so a
            // single hit renders DC at exactly dcLevel * vel and overlapping
            // hits stack in multiples of it.
            const float vel = pattern.stepVelocity (0, 0, 0);
            for (int s : { 4, 8, 12 })
                expectEquals (pattern.stepVelocity (0, s, 0), vel,
                              "Electronic four-on-the-floor kicks are authored uniform");
            const float singleHit = dcLevel * vel;

            const int blockSize = 512;   // deliberately does not divide patternLen,
                                          // so hits also land mid-block at times.

            juce::AudioBuffer<float> captured (1, patternLen);
            captured.clear();

            juce::AudioBuffer<float> block (1, blockSize);
            int playhead = 0;
            int written  = 0;
            while (written < patternLen)
            {
                block.clear();
                sampler.processBlock (block, blockSize, sr, pattern, bpm, playhead, 0);

                const int toCopy = juce::jmin (blockSize, patternLen - written);
                captured.copyFrom (0, written, block, 0, 0, toCopy);

                written  += toCopy;
                playhead  = (playhead + blockSize) % patternLen;
            }

            const float* d = captured.getReadPointer (0);
            float maxSample  = 0.0f;
            bool  allFinite  = true;
            for (int i = 0; i < patternLen; ++i)
            {
                if (! std::isfinite (d[i]))
                    allFinite = false;
                maxSample = juce::jmax (maxSample, d[i]);
            }

            expect (allFinite, "captured buffer must be finite throughout");

            logMessage ("B2 overlap test: single-hit level = " + juce::String (singleHit, 4)
                        + ", observed max sample = " + juce::String (maxSample, 4));
            expectGreaterThan (maxSample, 1.5f * singleHit,
                               "overlapping kicks must sum above a single hit's level — this "
                               "only happens if multiple instances of the same voice can ring "
                               "concurrently");

            kitRoot.deleteRecursively();
        }
    }

private:
    // Creates <temp>/adk_test_kit_XXXX/kick/kick.wav containing `numSamples`
    // of DC = `dcLevel` (mono). Returns the kit root directory.
    juce::File makeTempKitWithKick (int numSamples, double sr, float dcLevel = 1.0f)
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
                    buf.setSample (0, i, dcLevel);
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
