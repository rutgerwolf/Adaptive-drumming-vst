#include <JuceHeader.h>
#include "drummer/DrumStepClock.h"
#include "drummer/DrumPattern.h"

#include <vector>

/**
 * Guard for the shared step clock (forEachStepTrigger in DrumStepClock.h).
 *
 * The boundary-detection math is deliberately the same per-sample scan against
 * integer-truncated lengths it has always been (the exact-double rewrite is
 * separate, deferred work); what changed with the GrooveTable is *what happens
 * at a boundary*: the pattern is queried per voice for fire + velocity and the
 * callback carries velocity01. These tests therefore pin, free-running over
 * full bars across a sweep of block sizes:
 *   - every (step, voice) the pattern says fires does so exactly once per bar;
 *   - nothing the pattern doesn't fire is triggered — in particular no
 *     callback maps to step index >= kSteps (no phantom 17th step);
 *   - the velocity passed to the callback is the pattern's stepVelocity;
 *   - the full trigger sequence (positions, voices, velocities — including
 *     hash-gated ornament cells and their per-bar variation) is identical
 *     however the timeline is sliced into blocks.
 */
class DrumStepClockTest : public juce::UnitTest
{
public:
    DrumStepClockTest() : juce::UnitTest ("DrumStepClock", "drummer") {}

    struct Combo { DrumPattern::Style style; DrumPattern::Density density; const char* name; };

    void runTest() override
    {
        const Combo combos[] = {
            { DrumPattern::Style::Rock,       DrumPattern::Density::Medium, "Rock/Medium" },
            { DrumPattern::Style::Electronic, DrumPattern::Density::Full,   "Electronic/Full" },
            { DrumPattern::Style::Jazz,       DrumPattern::Density::Sparse, "Jazz/Sparse" },
        };
        // 130 BPM @ 44.1 kHz gives a non-integral bar (81415.38 samples) — the case
        // that stresses block-edge handling; 120 BPM @ 48 kHz is exactly divisible.
        const double tempos[][2] = { { 130.0, 44100.0 }, { 120.0, 48000.0 } };
        const int    blockSizes[] = { 1, 64, 480, 4096 };

        for (const auto& c : combos)
            for (const auto& t : tempos)
                for (int bs : blockSizes)
                    checkBar (c, t[0], t[1], bs);

        checkBlockSplitInvariance();
    }

private:
    void checkBar (const Combo& c, double bpm, double sr, int blockSize)
    {
        beginTest (juce::String ("one free-running bar — ") + c.name
                   + " @ " + juce::String (bpm, 0) + "/" + juce::String (sr, 0)
                   + " block " + juce::String (blockSize));

        DrumPattern p;
        p.loadStyle  (c.style);
        p.setDensity (c.density);

        const int patternLen = p.getLengthInSamples (bpm, sr);
        const int stepLen     = patternLen / DrumPattern::kSteps;
        expectGreaterThan (patternLen, 0);
        expectGreaterThan (stepLen, 0);

        constexpr int kVoices = DrumPattern::kNumVoices;
        int  hitCount[DrumPattern::kSteps][kVoices] = {};
        int  overrun     = 0;   // callbacks mapping to a step index outside [0, kSteps)
        int  badVelocity = 0;   // callbacks whose velocity != the pattern's stepVelocity

        // Scan exactly one bar's worth of samples (positions 0..patternLen-1
        // once, so every trigger belongs to bar index 0).
        int playhead = 0;
        int scanned  = 0;
        while (scanned < patternLen)
        {
            const int n = juce::jmin (blockSize, patternLen - scanned);
            forEachStepTrigger (p, playhead, n, bpm, sr, 0,
                [&] (int voice, int offset, float velocity01)
                {
                    const long long absPos = (long long) playhead + offset;
                    const int pos  = (int) (absPos % patternLen);
                    const int step = pos / stepLen;
                    if (step >= 0 && step < DrumPattern::kSteps && voice >= 0 && voice < kVoices)
                    {
                        ++hitCount[step][voice];
                        if (! juce::exactlyEqual (velocity01, p.stepVelocity (voice, step, 0)))
                            ++badVelocity;
                    }
                    else
                    {
                        ++overrun;
                    }
                });
            playhead += n;
            scanned  += n;
        }

        expectEquals (overrun, 0, "no callback may map past the 16th step");
        expectEquals (badVelocity, 0, "callback velocity must equal the pattern's stepVelocity");

        for (int s = 0; s < DrumPattern::kSteps; ++s)
            for (int v = 0; v < kVoices; ++v)
            {
                expect (hitCount[s][v] <= 1, "each (step, voice) fires at most once per bar");
                expectEquals (hitCount[s][v], p.stepFires (v, s, 0) ? 1 : 0,
                              "fired (step, voice) set must match the pattern's firing rule (step "
                              + juce::String (s) + ", voice " + juce::String (v) + ")");
            }
    }

    void checkBlockSplitInvariance()
    {
        beginTest ("trigger sequence is invariant to block slicing (incl. ornaments, 2 bars)");

        // Electronic at full complexity keeps hash-gated ornament cells in
        // range, so this also proves the bar index a trigger hashes with never
        // depends on where block edges (or the mid-block bar wrap) fall.
        const double bpm = 130.0, sr = 44100.0;

        DrumPattern p;
        p.loadStyle  (DrumPattern::Style::Electronic);
        p.setDensity (DrumPattern::Density::Full);

        const int patternLen = p.getLengthInSamples (bpm, sr);
        const int totalSamples = patternLen * 2;   // 2 bars → bar wrap mid-scan

        struct Trig
        {
            long long pos; int voice; float vel;
            bool operator== (const Trig& o) const
            {
                return pos == o.pos && voice == o.voice && juce::exactlyEqual (vel, o.vel);
            }
        };

        auto capture = [&] (int blockSize)
        {
            std::vector<Trig> seq;
            int playhead = 0;
            while (playhead < totalSamples)
            {
                const int n = juce::jmin (blockSize, totalSamples - playhead);
                // playhead runs unwrapped over both bars; the clock derives the
                // per-trigger bar index from it (base bar index 0).
                forEachStepTrigger (p, playhead, n, bpm, sr, 0,
                    [&] (int voice, int offset, float velocity01)
                    { seq.push_back ({ (long long) playhead + offset, voice, velocity01 }); });
                playhead += n;
            }
            return seq;
        };

        const auto reference = capture (1);   // sample-by-sample ground truth
        expectGreaterThan ((int) reference.size(), DrumPattern::kSteps,
                           "two full bars must produce a healthy number of triggers");

        // The two bars must not be identical trigger-for-trigger: ornament
        // cells hash per bar (backbone cells of course repeat).
        {
            std::vector<Trig> bar1, bar2;
            for (const auto& t : reference)
                (t.pos < patternLen ? bar1 : bar2).push_back (t);
            bool differs = bar1.size() != bar2.size();
            for (size_t i = 0; ! differs && i < bar1.size(); ++i)
                differs = bar1[i].voice != bar2[i].voice
                       || bar1[i].pos + patternLen != bar2[i].pos
                       || ! juce::exactlyEqual (bar1[i].vel, bar2[i].vel);
            expect (differs, "consecutive bars must vary through the per-bar hash");
        }

        for (int blockSize : { 64, 333, 480, 4096 })
        {
            const auto seq = capture (blockSize);
            expect (seq == reference,
                    "block size " + juce::String (blockSize)
                    + " must reproduce the sample-by-sample trigger sequence exactly");
        }
    }
};

static DrumStepClockTest drumStepClockTest;
