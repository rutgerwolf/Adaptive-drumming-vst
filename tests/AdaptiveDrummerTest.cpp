#include <JuceHeader.h>
#include "drummer/AdaptiveDrummer.h"
#include "drummer/TableExchange.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace
{
    /** A maximally-distinguishable dynamic GrooveTable for the TableExchange
        bar-latch tests below: snare fires at full velocity on every one of
        the 16 steps, deterministically (probability 255, complexityThreshold
        0 so it fires regardless of the complexity axis). No built-in style
        authors anything remotely this dense, so its effect on rendered audio
        is unmistakable. All other voices are left silent (velHigh 0) to keep
        the signal isolated to the one voice being asserted on. */
    GrooveTable makeAllStepsSnareTable() noexcept
    {
        GrooveTable t {};
        t.swing     = 0;
        t.styleSalt = 0x44594E31u;   // 'DYN1' — arbitrary, just needs to differ from the built-ins
        for (int s = 0; s < GrooveTable::kNumSteps; ++s)
            t.cells[1 /* Snare */][s] = { 127, 127, 0, 255, 0, StepCell::kFlagAccent };
        return t;
    }

    /** FNV-1a over the raw bit patterns of every sample — a cheap, exact
        (no floating tolerance) fingerprint of a render, used as a fixed
        golden value so a future change that silently perturbs bit-identical
        output gets caught, not just a same-process double-render. */
    uint64_t hashBuffer (const juce::AudioBuffer<float>& buf, int numSamples, uint64_t h = 0xcbf29ce484222325ull)
    {
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        {
            const float* d = buf.getReadPointer (ch);
            for (int i = 0; i < numSamples; ++i)
            {
                uint32_t bits;
                std::memcpy (&bits, &d[i], sizeof (bits));
                h ^= bits;
                h *= 0x100000001b3ull;
            }
        }
        return h;
    }

    // Writes a mono WAV of `numSamples` containing DC = `dcLevel` and returns
    // a freshly-created kit root directory <temp>/.../kick/kick.wav, matching
    // DrumSamplerTest.cpp's makeTempKitWithKick idiom. Used only by the
    // determinism regression test below: DrumSynth's noise voices draw from
    // an unseeded juce::Random (a pre-existing property, unrelated to this
    // PR), so they are not bit-reproducible across instances/runs — a real
    // bit-identical check needs the sample engine, which has no RNG at all.
    juce::File makeTempKickKit (double sr)
    {
        auto root = juce::File::createTempFile ("adk_tableexchange_kit");
        root.deleteFile();
        root.createDirectory();

        auto kickDir = root.getChildFile ("kick");
        kickDir.createDirectory();

        const int numSamples = (int) (sr * 0.2);   // 200 ms

        juce::WavAudioFormat fmt;
        auto file = kickDir.getChildFile ("kick.wav");
        file.deleteFile();
        if (auto* os = file.createOutputStream().release())
        {
            std::unique_ptr<juce::AudioFormatWriter> writer (fmt.createWriterFor (os, sr, 1, 16, {}, 0));
            if (writer != nullptr)
            {
                juce::AudioBuffer<float> buf (1, numSamples);
                for (int i = 0; i < numSamples; ++i)
                    buf.setSample (0, i, 1.0f);   // deterministic DC "hit"
                writer->writeFromAudioSampleBuffer (buf, 0, numSamples);
            }
            else
            {
                delete os;
            }
        }

        return root;
    }
}

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

        beginTest ("TableExchange — a dynamic table publish bar-latches, same as a style switch (roadmap Step 19)");
        {
            // getActiveTable() exposes the exact pointer DrumPattern is
            // currently rendering from — either a built-in style's const
            // table, or (once adopted) the TableExchange slot itself. Pointer
            // identity is an exact, single-instance observable, so this test
            // doesn't need to diff raw audio between two engines (DrumSynth's
            // cymbal/noise voices draw from an unseeded juce::Random, so two
            // separate instances are not bit-comparable — a pre-existing
            // property, unrelated to this PR; see the determinism test below
            // for how that's worked around when a real audio check is needed).
            const double sr  = 48000.0;
            const double bpm = 120.0;             // bar = 96000 samples exactly
            const int blockSize = 512;

            AdaptiveDrummer d;
            d.prepare (sr, blockSize);
            d.setBpm (bpm);
            d.setHostTimeline (false, false, 0.0);
            d.setUseSynth (true);
            d.setStyle (DrumPattern::Style::Rock);
            d.setDensity (DrumPattern::Density::Full);

            DrumPattern refRock;
            refRock.loadStyle (DrumPattern::Style::Rock);
            const GrooveTable* rockTablePtr = &refRock.getActiveTable();

            DrumPattern refPattern;
            const int patternLen = refPattern.getLengthInSamples (bpm, sr);
            expectEquals (patternLen, 96000);
            const int fullBlocksInBar = patternLen / blockSize;   // 187

            juce::AudioBuffer<float> buf (2, blockSize);
            auto renderBlocks = [&] (int count)
            {
                for (int b = 0; b < count; ++b) { buf.clear(); d.processBlock (buf, blockSize); }
            };

            renderBlocks (10);
            expect (&d.getActiveTable() == rockTablePtr, "still on the built-in Rock table 10 blocks in");

            GrooveTable dynTable = makeAllStepsSnareTable();
            GrooveTable* slot = d.getTableExchange().tryStage();
            expect (slot != nullptr, "tryStage must succeed on a fresh mailbox");
            *slot = dynTable;
            d.getTableExchange().publish();

            // Publishing mid-bar must not be observable yet: consume() only
            // happens at the step-0 bar wrap.
            renderBlocks (fullBlocksInBar - 10);
            expectEquals ((int) d.getBarIndex(), 0, "still inside bar 0");
            expect (&d.getActiveTable() == rockTablePtr,
                    "a mid-bar publish must not affect the active table before the next bar wrap");

            // This block spans the wrap (95744 -> 96256): the dynamic table
            // is adopted exactly at the in-block step-0 boundary.
            renderBlocks (1);
            expectEquals ((int) d.getBarIndex(), 1, "bar counter advances at the wrap");
            expect (&d.getActiveTable() == slot,
                    "the dynamic table must be adopted exactly at the wrap");

            // Sanity: the newly-active table (snare firing at full velocity on
            // every step) must actually be audible.
            buf.clear();
            d.processBlock (buf, blockSize);
            expectGreaterThan (buf.getMagnitude (0, blockSize), 0.01f,
                                "the adopted dynamic table must produce audible output");
        }

        beginTest ("TableExchange — a style flip drops the dynamic table and frees both slots (roadmap Step 19)");
        {
            const double sr  = 48000.0;
            const double bpm = 120.0;
            const int blockSize = 512;

            AdaptiveDrummer d;
            d.prepare (sr, blockSize);
            d.setBpm (bpm);
            d.setHostTimeline (false, false, 0.0);
            d.setUseSynth (true);
            d.setStyle (DrumPattern::Style::Rock);
            d.setDensity (DrumPattern::Density::Full);

            DrumPattern refPattern;
            const int patternLen      = refPattern.getLengthInSamples (bpm, sr);
            const int fullBlocksInBar = patternLen / blockSize;   // 187

            juce::AudioBuffer<float> buf (2, blockSize);
            auto renderBlocks = [&] (int count)
            {
                for (int b = 0; b < count; ++b) { buf.clear(); d.processBlock (buf, blockSize); }
            };

            // Publish before the very first processBlock(): the playhead
            // starts at step 0, so (mirroring the existing "a style requested
            // before playback starts" precedent above) the dynamic table is
            // adopted immediately on the first call, still within bar 0.
            GrooveTable dynTable = makeAllStepsSnareTable();
            GrooveTable* slot = d.getTableExchange().tryStage();
            *slot = dynTable;
            d.getTableExchange().publish();

            renderBlocks (1);
            expectEquals ((int) d.getBarIndex(), 0, "adopting on the very first block does not itself wrap a bar");
            expect (&d.getActiveTable() == slot, "dynamic table adopted immediately (playhead was already at step 0)");

            // Request a style flip while the dynamic table is active, and let
            // it ride out the rest of bar 0 (bar-latched, same as any style
            // switch — the dynamic table must not be dropped until the wrap).
            d.setStyle (DrumPattern::Style::Jazz);
            renderBlocks (fullBlocksInBar - 1);   // total so far: fullBlocksInBar blocks = 95744 samples
            expectEquals ((int) d.getBarIndex(), 0, "still inside bar 0 — the flip must not jump mid-bar");
            expect (&d.getActiveTable() == slot, "dynamic table must still be active right up to the wrap");

            renderBlocks (1);   // crosses the wrap (95744 -> 96256): the flip (and the drop) happens here
            expectEquals ((int) d.getBarIndex(), 1, "bar counter advances at the wrap");
            expect (d.getStyle() == DrumPattern::Style::Jazz,
                    "the requested style must be live at the wrap, dropping the dynamic table");

            DrumPattern refJazz;
            refJazz.loadStyle (DrumPattern::Style::Jazz);
            expect (&d.getActiveTable() == &refJazz.getActiveTable(),
                    "the active table must now be Jazz's built-in const table, not the dynamic slot");

            // releaseActive() must have fired as part of the flip: both slots
            // must be independently stageable again, exactly as with a fresh
            // mailbox (mirrors TableExchangeTest's "both slots held" proof).
            TableExchange& tx = d.getTableExchange();
            GrooveTable* q1 = tx.tryStage();
            expect (q1 != nullptr, "the first slot must be stageable after the style flip released the dynamic table");
            *q1 = makeAllStepsSnareTable();
            tx.publish();
            GrooveTable* q2 = tx.tryStage();
            expect (q2 != nullptr && q2 != q1,
                    "the second slot must also be independently stageable — proving releaseActive() fired, "
                    "not just that the mailbox happens to still hold one free slot");
        }

        beginTest ("TableExchange — mailbox never touched: 8-bar render is unperturbed (regression guard)");
        {
            // The headline safety property of this PR: wiring tableExchange
            // .consume() into every bar-wrap site must be a complete no-op for
            // every existing caller, since nothing publishes into it yet.
            //
            // This needs the sample engine (DrumSampler), not the synth: the
            // synth's cymbal/noise voices draw from an unseeded juce::Random
            // member (a pre-existing property of DrumSynth, unrelated to this
            // PR), so even two untouched, identically-configured synth
            // renders are not bit-reproducible — that would falsely look like
            // a regression here. DrumSampler has no RNG anywhere, so a tiny
            // deterministic local test kit gives a genuine bit-for-bit check.
            const double sr  = 44100.0;
            const double bpm = 120.0;
            const int blockSize = 512;

            auto kitRoot = makeTempKickKit (sr);

            DrumPattern refPattern;
            const int patternLen = refPattern.getLengthInSamples (bpm, sr);
            const juce::int64 totalSamples = (juce::int64) patternLen * 8;   // 8 bars

            auto renderAndHash = [&] () -> uint64_t
            {
                AdaptiveDrummer d;
                d.prepare (sr, blockSize);
                d.setUseSynth (false);
                expect (d.loadSamples (kitRoot), "the local deterministic test kit must load");
                d.setBpm (bpm);
                d.setStyle (DrumPattern::Style::Rock);
                d.setDensity (DrumPattern::Density::Full);
                d.setHostTimeline (false, false, 0.0);

                juce::AudioBuffer<float> buf (2, blockSize);
                uint64_t h = 0xcbf29ce484222325ull;
                juce::int64 rendered = 0;
                while (rendered < totalSamples)
                {
                    const int n = (int) juce::jmin ((juce::int64) blockSize, totalSamples - rendered);
                    buf.setSize (2, n, false, false, true);
                    buf.clear();
                    d.processBlock (buf, n);
                    h = hashBuffer (buf, n, h);
                    rendered += n;
                }
                return h;
            };

            const uint64_t hashA = renderAndHash();
            const uint64_t hashB = renderAndHash();
            expectEquals ((juce::int64) hashA, (juce::int64) hashB,
                          "two identically-configured drummers (neither ever touching the mailbox) "
                          "must render bit-for-bit identically over 8 bars");

            // Golden value captured from this exact render immediately after
            // PR 19a landed (TableExchange wired in, dormant, no producer).
            constexpr uint64_t kGoldenHash = 0x58ad1625c157ede5ull;
            expectEquals ((juce::int64) hashA, (juce::int64) kGoldenHash,
                          "8-bar Rock/Full render must be bit-identical to the pre-recorded golden hash — "
                          "any difference means the dormant TableExchange wiring perturbed existing behaviour");

            kitRoot.deleteRecursively();
        }
    }
};

static AdaptiveDrummerTest adaptiveDrummerTest;
