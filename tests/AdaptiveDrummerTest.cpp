#include <JuceHeader.h>
#include "drummer/AdaptiveDrummer.h"

#include <cmath>

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

        beginTest ("synth free-run produces audio (no samples needed)");
        {
            AdaptiveDrummer d;
            d.prepare (44100.0, 512);
            d.setUseSynth (true);
            d.setStyle (DrumPattern::Style::Rock);
            d.setDensity (DrumPattern::Density::Full);
            d.setHostTimeline (false, false, 0.0);   // free-run, as in Standalone

            juce::AudioBuffer<float> buf (2, 512);
            float peak = 0.0f;
            for (int b = 0; b < 200; ++b)     // ~2.3 s, several bars
            {
                buf.clear();
                d.processBlock (buf, 512);
                peak = juce::jmax (peak, buf.getMagnitude (0, 512));
            }
            expectGreaterThan (peak, 0.01f);  // the synth must actually sound
        }

        beginTest ("host-sync — playhead tiles blocks exactly over a non-integral-bar tempo");
        {
            // 130 BPM @ 44.1 kHz: one bar is 81415.3846.. samples, which
            // getLengthInSamples() truncates to 81415 (Finding #1 / B4xB3).
            // A host feeding a steadily-advancing ppq every block used to make
            // AdaptiveDrummer re-derive the playhead fresh each block against
            // that truncated length, so consecutive blocks did not tile
            // perfectly — this is a direct, root-cause check of that tiling,
            // independent of pattern/voice content.
            const double sr  = 44100.0;
            const double bpm = 130.0;

            DrumPattern refPattern;   // only used to read the (identical) pattern length
            refPattern.loadStyle (DrumPattern::Style::Rock);
            const int patternLen = refPattern.getLengthInSamples (bpm, sr);
            expectGreaterThan (patternLen, 0);

            for (int blockSize : { 480, 4096 })
            {
                AdaptiveDrummer d;
                d.prepare (sr, blockSize);
                d.setBpm  (bpm);

                const double ppqPerSample = (bpm / 60.0) / sr;   // quarter-notes per sample
                const juce::int64 totalSamples = (juce::int64) patternLen * 8;   // 8 bars

                double         expectedPlayhead = 0.0;   // independent perfect-tiling reference
                double         hostPpq          = 0.0;
                juce::int64    rendered         = 0;
                bool           anyMismatch      = false;

                juce::AudioBuffer<float> buf (2, blockSize);

                while (rendered < totalSamples)
                {
                    const int n = (int) juce::jmin ((juce::int64) blockSize, totalSamples - rendered);

                    buf.setSize (2, n, false, false, true);
                    buf.clear();
                    d.setHostTimeline (true, true, hostPpq);
                    d.processBlock (buf, n);

                    const int playheadUsed = d.getCurrentPlayheadSample();
                    if (std::abs ((double) playheadUsed - expectedPlayhead) > 0.5)
                        anyMismatch = true;

                    expectedPlayhead = std::fmod (expectedPlayhead + (double) n, (double) patternLen);
                    rendered += n;
                    hostPpq  += (double) n * ppqPerSample;
                }

                expect (! anyMismatch,
                        "playhead must tile blocks with no gap/overlap (block size "
                        + juce::String (blockSize) + ")");
            }
        }

        beginTest ("host-sync — small ppq jitter around the exact rate does not force a mismatch");
        {
            // A host's reported ppq is not always a mathematically perfect
            // ramp; tiny jitter (well under the ~2 ms resync tolerance) must
            // not cause the playhead to depart from free-running tiling.
            const double sr  = 44100.0;
            const double bpm = 130.0;
            const int    blockSize = 512;

            DrumPattern refPattern;
            refPattern.loadStyle (DrumPattern::Style::Rock);
            const int patternLen = refPattern.getLengthInSamples (bpm, sr);

            AdaptiveDrummer d;
            d.prepare (sr, blockSize);
            d.setBpm  (bpm);

            const double ppqPerSample = (bpm / 60.0) / sr;
            double expectedPlayhead = 0.0;
            double hostPpq          = 0.0;
            bool   anyMismatch      = false;

            juce::AudioBuffer<float> buf (2, blockSize);
            juce::Random rng (12345);

            for (int b = 0; b < 200; ++b)
            {
                // Jitter the reported ppq by up to +/-0.25 samples worth — far
                // below the ~2 ms (~88 sample) resync tolerance at this rate.
                const double jitterSamples = (rng.nextDouble() - 0.5) * 0.5;
                const double jitteredPpq   = hostPpq + jitterSamples * ppqPerSample;

                buf.clear();
                d.setHostTimeline (true, true, jitteredPpq);
                d.processBlock (buf, blockSize);

                const int playheadUsed = d.getCurrentPlayheadSample();
                if (std::abs ((double) playheadUsed - expectedPlayhead) > 0.5)
                    anyMismatch = true;

                expectedPlayhead = std::fmod (expectedPlayhead + (double) blockSize, (double) patternLen);
                hostPpq         += (double) blockSize * ppqPerSample;
            }

            expect (! anyMismatch, "sub-tolerance ppq jitter must not disturb block tiling");
        }

        beginTest ("host-sync — playing without a ppq position free-runs instead of resetting");
        {
            // Finding #6: a host that reports isPlaying without a ppq position
            // must not be treated as stopped; setHostTimeline's hasPpqPosition
            // flag lets AdaptiveDrummer free-run in that case.
            AdaptiveDrummer d;
            d.prepare (44100.0, 512);
            d.setStyle (DrumPattern::Style::Rock);
            d.setDensity (DrumPattern::Density::Full);

            juce::AudioBuffer<float> buf (2, 512);
            float peak = 0.0f;
            for (int b = 0; b < 200; ++b)
            {
                buf.clear();
                d.setHostTimeline (true, false, 0.0);   // playing, but no ppq reported
                d.processBlock (buf, 512);
                peak = juce::jmax (peak, buf.getMagnitude (0, 512));
            }
            expectGreaterThan (peak, 0.01f);
        }

        beginTest ("style change latches at the bar wrap, never mid-bar");
        {
            // A style request mid-bar must not restructure the groove until the
            // playhead reaches step 0 of the next bar (structural changes are
            // bar-latched; ARCHITECTURE_2D_FOLLOW §2.5). getStyle() reports the
            // *active* table, so it is the direct observable.
            const double sr  = 48000.0;
            const double bpm = 120.0;             // bar = 96000 samples exactly
            const int blockSize = 512;

            AdaptiveDrummer d;
            d.prepare (sr, blockSize);
            d.setBpm (bpm);
            d.setHostTimeline (false, false, 0.0);

            DrumPattern ref;
            const int patternLen = ref.getLengthInSamples (bpm, sr);
            expectEquals (patternLen, 96000);

            juce::AudioBuffer<float> buf (2, blockSize);
            auto renderBlocks = [&] (int count)
            {
                for (int b = 0; b < count; ++b)
                {
                    buf.clear();
                    d.processBlock (buf, blockSize);
                }
            };

            expect (d.getStyle() == DrumPattern::Style::Rock, "default style is Rock");
            expectEquals ((int) d.getBarIndex(), 0);

            renderBlocks (10);                    // 5120 samples into bar 0
            d.setStyle (DrumPattern::Style::Jazz);   // requested mid-bar...

            // ...and must stay pending for the whole rest of the bar. 187 full
            // blocks fit inside the 96000-sample bar (187 × 512 = 95744).
            const int fullBlocksInBar = patternLen / blockSize;   // 187
            renderBlocks (fullBlocksInBar - 10);                  // playhead now at 95744
            expect (d.getStyle() == DrumPattern::Style::Rock,
                    "style must not switch mid-bar");
            expectEquals ((int) d.getBarIndex(), 0, "still inside bar 0");

            // The next block spans the wrap (95744 → 96256): the switch happens
            // exactly at the in-block step-0 boundary.
            renderBlocks (1);
            expect (d.getStyle() == DrumPattern::Style::Jazz,
                    "style must be applied at the step-0 bar wrap");
            expectEquals ((int) d.getBarIndex(), 1, "bar counter advances at the wrap");

            // A request made while the playhead sits at step 0 (fresh reset)
            // applies before the first trigger — no one-bar lag from silence.
            AdaptiveDrummer d2;
            d2.prepare (sr, blockSize);
            d2.setBpm (bpm);
            d2.setHostTimeline (false, false, 0.0);
            d2.setStyle (DrumPattern::Style::Electronic);
            buf.clear();
            d2.processBlock (buf, blockSize);
            expect (d2.getStyle() == DrumPattern::Style::Electronic,
                    "a style requested before playback starts must apply at bar 0");
        }
    }
};

static AdaptiveDrummerTest adaptiveDrummerTest;
