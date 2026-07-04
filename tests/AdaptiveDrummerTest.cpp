#include <JuceHeader.h>
#include "drummer/AdaptiveDrummer.h"

#include <cmath>
#include <limits>

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

        beginTest ("host-sync — setHostTimeline's beatsPerBar threads into the ppq resync (not hardcoded 4/4)");
        {
            // Confirmed gap: setHostTimeline never accepted a beatsPerBar, so
            // a 3/4 (or any non-4/4) host always resynced as if the bar were
            // 4 beats long — the downbeat drifts relative to the host's own
            // bar line. ppq = 3.0 is a full bar boundary in 3/4 (the resync
            // target should be step 0 of our pattern) but 3/4 of the way
            // through a 4/4 bar (the old, always-4/4 mapping would target
            // ~75% into the pattern instead).
            const double sr  = 44100.0;
            const double bpm = 120.0;

            DrumPattern refPattern;
            const int patternLen = refPattern.getLengthInSamples (bpm, sr);
            expectGreaterThan (patternLen, 0);

            AdaptiveDrummer d;
            d.prepare (sr, 512);
            d.setBpm  (bpm);

            juce::AudioBuffer<float> buf (2, 512);
            buf.clear();
            d.setHostTimeline (true, true, 3.0, 3.0);   // 3/4, exactly at a bar boundary
            d.processBlock (buf, 512);

            const int playheadAfter = d.getCurrentPlayheadSample();
            const int nearZeroBand  = patternLen / 8;
            const bool nearDownbeat = playheadAfter < nearZeroBand
                                   || playheadAfter > patternLen - nearZeroBand;
            expect (nearDownbeat,
                    "beatsPerBar=3.0 must resync toward the 3/4 downbeat (near 0), not the "
                    "~75%-into-the-pattern position a hardcoded 4/4 assumption would produce "
                    "(playhead landed at " + juce::String (playheadAfter) + " / " + juce::String (patternLen) + ")");
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

        beginTest ("stop mid-decay — renderTails lets a ringing voice decay naturally (Step 8)");
        {
            // Rock/Full guarantees a crash on step 0 (DrumSynth's ~1.2 s noise
            // decay) — plenty of tail to observe. rewindPlayhead() + renderTails()
            // is what PluginProcessor now calls instead of reset() every stopped
            // block; this pins that the tail keeps sounding for a while after the
            // stop edge (the old reset()-every-block code went silent on the very
            // next block) and that its envelope only ever decays, never jumps back
            // up, before eventually settling to silence. (A raw sample-to-sample
            // delta check isn't meaningful here — the crash voice is differentiated
            // white noise, whose adjacent samples are large and uncorrelated by
            // design, stopped or not; the envelope is the honest signal.)
            const double sr        = 44100.0;
            const int    blockSize = 512;

            AdaptiveDrummer d;
            d.prepare (sr, blockSize);
            d.setUseSynth (true);
            d.setStyle    (DrumPattern::Style::Rock);
            d.setDensity  (DrumPattern::Density::Full);
            d.setHostTimeline (false, false, 0.0);   // free-run, like Standalone

            juce::AudioBuffer<float> buf (2, blockSize);
            buf.clear();
            d.processBlock (buf, blockSize);   // triggers step 0 (crash etc.)
            expectGreaterThan (buf.getMagnitude (0, blockSize), 0.01f,
                               "step 0 hit must be audible before stopping");

            // Simulate the transport-stop edge: rewind once, then keep decaying.
            d.rewindPlayhead();

            const int tailBlocks = 200;   // ~2.3 s — longer than the ~1.2 s crash decay
            float     peakSoFar  = std::numeric_limits<float>::max();
            bool      everAudibleRightAfterStop = false;
            bool      everSilent                = false;
            bool      envelopeRoseAfterDecaying = false;

            for (int b = 0; b < tailBlocks; ++b)
            {
                buf.clear();
                d.renderTails (buf, blockSize);
                const float blockPeak = buf.getMagnitude (0, blockSize);

                if (b < 3 && blockPeak > 0.01f)
                    everAudibleRightAfterStop = true;

                // Once the envelope has clearly started decaying (dropped well
                // below the initial hit), it must never climb back toward that
                // initial level — that would mean a voice got re-triggered
                // instead of only ever decaying.
                if (peakSoFar < 0.1f && blockPeak > peakSoFar * 4.0f)
                    envelopeRoseAfterDecaying = true;
                peakSoFar = juce::jmin (peakSoFar, blockPeak);

                if (blockPeak < 1.0e-4f)
                    everSilent = true;
            }

            expect (everAudibleRightAfterStop,
                    "the tail must still be audible for the first few blocks after stopping, "
                    "not cut to silence on the very next block");
            expect (! envelopeRoseAfterDecaying,
                    "the envelope must only ever decay while stopped, never jump back up");
            expect (everSilent, "the tail must eventually decay to silence");
        }

        beginTest ("toggling source mid-decay resets the deactivated engine (Step 8)");
        {
            // setUseSynth() must silence whichever engine is switched away from,
            // so a toggle-and-back doesn't resume a frozen tail. renderTails()
            // never fires new triggers, so any sound it produces right after the
            // toggle-back can only be a leftover resumed fragment.
            const double sr        = 44100.0;
            const int    blockSize = 512;

            AdaptiveDrummer d;
            d.prepare (sr, blockSize);
            d.setUseSynth (true);
            d.setStyle    (DrumPattern::Style::Rock);
            d.setDensity  (DrumPattern::Density::Full);
            d.setHostTimeline (false, false, 0.0);

            juce::AudioBuffer<float> buf (2, blockSize);
            buf.clear();
            d.processBlock (buf, blockSize);   // triggers step 0, synth now ringing
            expectGreaterThan (buf.getMagnitude (0, blockSize), 0.01f,
                               "step 0 hit must be audible before toggling");

            d.setUseSynth (false);   // switch away from synth mid-decay
            d.setUseSynth (true);    // ...and back, before the crash would have finished

            buf.clear();
            d.renderTails (buf, blockSize);
            expectEquals (buf.getMagnitude (0, blockSize), 0.0f,
                          "toggling away and back must reset the synth, not resume a frozen tail");
        }
    }
};

static AdaptiveDrummerTest adaptiveDrummerTest;
