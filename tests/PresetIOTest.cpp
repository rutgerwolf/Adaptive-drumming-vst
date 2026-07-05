#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PresetIO.h"

#include <cstring>

/**
 * Tests for the user-facing preset format (.adpreset JSON) and the MIDI
 * groove importer — see PresetIO.h. Kept lean per the project's testing
 * policy: round-trips, the reject-garbage path, and the two genuinely risky
 * MIDI behaviours (quantization/first-bar selection and the GM family map).
 */
class PresetIOTest : public juce::UnitTest
{
public:
    PresetIOTest() : juce::UnitTest ("PresetIO", "processor") {}

    void runTest() override
    {
        juce::ScopedJuceInitialiser_GUI guiInit;

        beginTest ("preset JSON round-trip: style, axes, mutes, seed and every custom-grid cell");
        {
            AdaptiveDrummerProcessor src;

            // Non-default everything. Style FIRST (a later style pick would
            // discard the drawing — decision A), then draw.
            auto* styleParam = src.apvts.getParameter ("style");
            styleParam->setValueNotifyingHost (styleParam->convertTo0to1 (1.0f));   // Jazz
            src.publishDynamicTableIfNeeded();   // let the edge detector consume the pick

            auto* ip = src.apvts.getParameter ("intensity");
            auto* cp = src.apvts.getParameter ("complexity");
            ip->setValueNotifyingHost (ip->convertTo0to1 (0.8f));
            cp->setValueNotifyingHost (cp->convertTo0to1 (0.3f));

            src.setGridCell (0, 0,  true, 120);   // Kick
            src.setGridCell (5, 15, true, 33);    // Tom
            src.setGridCell (1, 4,  false, 0);    // clear a Jazz backbone cell
            src.setVoiceMuted (3, true);          // Crash
            src.randomizeGrooveSeed();
            const auto seed = src.getCurrentGrooveSeed();

            const auto json = src.createPresetJson ("Round Trip");

            AdaptiveDrummerProcessor dst;
            expect (dst.applyPresetJson (json), "a freshly-exported preset must import cleanly");

            expectEquals ((int) *dst.apvts.getRawParameterValue ("style"), 1,
                          "style must restore as Jazz");
            expectWithinAbsoluteError ((double) *dst.apvts.getRawParameterValue ("intensity"), 0.8, 0.001,
                                       "intensity must survive the round-trip");
            expectWithinAbsoluteError ((double) *dst.apvts.getRawParameterValue ("complexity"), 0.3, 0.001,
                                       "complexity must survive the round-trip");
            expectEquals ((int) dst.getMuteMask(), 1 << 3, "the crash mute must survive");
            expectEquals ((int) dst.getCurrentGrooveSeed(), (int) seed, "the groove seed must survive");

            expect (dst.hasCustomGrid(),
                    "the preset's custom grid must arrive as a custom grid — style-setting above "
                    "must not have discarded it via the style-pick edge detector");

            const GrooveTable& a = src.getGridTable();
            const GrooveTable& b = dst.getGridTable();
            expect (std::memcmp (a.cells, b.cells, sizeof (a.cells)) == 0,
                    "all 96 cells (every StepCell field) must survive the JSON round-trip");
            expectEquals ((int) b.swing, (int) a.swing, "swing must survive the round-trip");
            expectEquals ((int) b.styleSalt, (int) a.styleSalt,
                          "the re-salted grid must carry Jazz's salt, same as the hand-drawn original");
        }

        beginTest ("preset JSON round-trip without a custom grid clears an existing drawing");
        {
            AdaptiveDrummerProcessor src;
            auto* styleParam = src.apvts.getParameter ("style");
            styleParam->setValueNotifyingHost (styleParam->convertTo0to1 (2.0f));   // Electronic
            src.setVoiceMuted (0, true);
            src.setVoiceMuted (2, true);
            src.randomizeGrooveSeed();
            const auto seed = src.getCurrentGrooveSeed();
            expect (! src.hasCustomGrid(), "premise: the source has no custom grid");

            const auto json = src.createPresetJson ("No Grid");
            expect (juce::JSON::parse (json).getProperty ("customGrid", 1).isVoid(),
                    "a gridless preset must serialize customGrid as JSON null");

            AdaptiveDrummerProcessor dst;
            dst.setGridCell (0, 0, true, 100);   // a live drawing the import must clear
            expect (dst.applyPresetJson (json), "the gridless preset must import cleanly");

            expect (! dst.hasCustomGrid(),
                    "a gridless preset is the full sound — it must clear an existing custom grid");
            expectEquals ((int) *dst.apvts.getRawParameterValue ("style"), 2, "style must restore");
            expectEquals ((int) dst.getMuteMask(), 0x05, "both mutes must restore");
            expectEquals ((int) dst.getCurrentGrooveSeed(), (int) seed, "seed must restore");
        }

        beginTest ("corrupt JSON, unknown version and unknown style are rejected with no partial apply");
        {
            AdaptiveDrummerProcessor proc;
            proc.setVoiceMuted (1, true);
            proc.setGridCell (2, 7, true, 64);
            const auto maskBefore  = (int) proc.getMuteMask();
            const auto seedBefore  = (int) proc.getCurrentGrooveSeed();
            const auto styleBefore = (int) *proc.apvts.getRawParameterValue ("style");

            expect (! proc.applyPresetJson ("this is not json"),        "non-JSON must be rejected");
            expect (! proc.applyPresetJson ("{}"),                      "a JSON object without presetFormat must be rejected");
            expect (! proc.applyPresetJson ("[1, 2, 3]"),               "a non-object JSON document must be rejected");

            // A real export with the version bumped past what we understand.
            auto future = juce::JSON::parse (proc.createPresetJson ("x"));
            future.getDynamicObject()->setProperty ("presetFormat", kFutureVersion());
            expect (! proc.applyPresetJson (juce::JSON::toString (future)),
                    "a future presetFormat must be rejected, like the internal gridVersion check");

            auto badStyle = juce::JSON::parse (proc.createPresetJson ("x"));
            badStyle.getDynamicObject()->setProperty ("style", "Polka");
            expect (! proc.applyPresetJson (juce::JSON::toString (badStyle)),
                    "an unknown style name must be rejected");

            // A structurally-broken grid: right shape flag, wrong voice count.
            auto badGrid = juce::JSON::parse (proc.createPresetJson ("x"));
            badGrid.getDynamicObject()->setProperty ("customGrid",
                juce::JSON::parse ("{ \"swing\": 0, \"cells\": [[], []] }"));
            expect (! proc.applyPresetJson (juce::JSON::toString (badGrid)),
                    "a malformed customGrid must reject the whole preset");

            expectEquals ((int) proc.getMuteMask(), maskBefore,           "no partial apply: mutes untouched");
            expectEquals ((int) proc.getCurrentGrooveSeed(), seedBefore,  "no partial apply: seed untouched");
            expectEquals ((int) *proc.apvts.getRawParameterValue ("style"), styleBefore,
                          "no partial apply: style untouched");
            expect (proc.hasCustomGrid() && proc.getGridTable().cells[2][7].velHigh == 64,
                    "no partial apply: the drawing untouched");
        }

        beginTest ("MIDI import quantizes channel-10 notes into the custom grid (channel 10 wins over others)");
        {
            // tpqn 480, 4/4 → one bar = 1920 ticks, one 16th step = 120 ticks.
            const auto mid = writeTempMidi (480, {
                { 10, 36, 100, 0.0   },   // kick, step 0
                { 10, 38,  90, 480.0 },   // snare, beat 2 = step 4
                { 10, 42,  60, 840.0 },   // closed hat, step 7
                { 10, 42,  70, 850.0 },   // quantizes to the same cell — max velocity must win
                {  1, 38, 127, 240.0 },   // a MELODIC channel-1 event: must be ignored (channel 10 present)
            });

            AdaptiveDrummerProcessor proc;
            expect (proc.importMidiGroove (mid), "the import must succeed");
            expect (proc.hasCustomGrid(), "a MIDI import must land as an editable custom grid");

            const GrooveTable& t = proc.getGridTable();

            const StepCell& kick = t.cells[0][0];
            expectEquals ((int) kick.velLow,              100, "kick velLow must be the played velocity");
            expectEquals ((int) kick.velHigh,             100, "kick velHigh must equal velLow (hand-drawn convention)");
            expectEquals ((int) kick.complexityThreshold, 0,   "imported hits must always be eligible");
            expectEquals ((int) kick.probability,         255, "imported hits must be deterministic");
            expectEquals ((int) kick.velRandRange,        0,   "no humanization jitter on imported hits");
            expectEquals ((int) kick.flags,               0,   "no accent/ghost flags on imported hits");

            expectEquals ((int) t.cells[1][4].velHigh, 90, "the snare must quantize onto step 4");
            expectEquals ((int) t.cells[2][7].velHigh, 70,
                          "two notes quantizing to one cell must keep the higher velocity");
            expectEquals ((int) t.cells[1][2].velHigh, 0,
                          "with channel-10 notes present, other channels must be ignored");
            expectEquals ((int) t.swing, 0, "an imported groove is straight (swing 0)");

            mid.deleteFile();
        }

        beginTest ("GM family mapping + all-channels fallback + first-populated-bar selection");
        {
            // No channel-10 notes at all (hobbyist-pack style, drums on
            // channel 1) → every channel is scanned. All notes sit in BAR 1
            // (bar 0 is empty), plus one in bar 2 that must be ignored.
            // tpqn 96, 4/4 → bar = 384 ticks, step = 24 ticks.
            const auto mid = writeTempMidi (96, {
                { 1, 40,  80, 384.0 },   // Electric Snare (not our canonical 38) → Snare, step 0
                { 1, 35,  90, 408.0 },   // Acoustic Bass Drum → Kick, step 1
                { 1, 46,  70, 432.0 },   // Open Hi-Hat → HiHat, step 2
                { 1, 57,  99, 456.0 },   // Crash 2 → Crash, step 3
                { 1, 59,  88, 480.0 },   // Ride 2 → Ride, step 4
                { 1, 47,  77, 504.0 },   // Low-Mid Tom → Tom, step 5
                { 1, 56, 100, 528.0 },   // Cowbell: unmapped, must be dropped
                { 1, 36,  55, 768.0 },   // bar 2: outside the first populated bar, must be ignored
            });

            AdaptiveDrummerProcessor proc;
            expect (proc.importMidiGroove (mid), "the fallback (no channel 10) import must succeed");

            const GrooveTable& t = proc.getGridTable();
            expectEquals ((int) t.cells[1][0].velHigh, 80, "40 Electric Snare must map to the Snare voice");
            expectEquals ((int) t.cells[0][1].velHigh, 90, "35 Acoustic Bass Drum must map to the Kick voice");
            expectEquals ((int) t.cells[2][2].velHigh, 70, "46 Open Hi-Hat must map to the HiHat voice");
            expectEquals ((int) t.cells[3][3].velHigh, 99, "57 Crash 2 must map to the Crash voice");
            expectEquals ((int) t.cells[4][4].velHigh, 88, "59 Ride 2 must map to the Ride voice");
            expectEquals ((int) t.cells[5][5].velHigh, 77, "47 Low-Mid Tom must map to the Tom voice");

            expectEquals ((int) t.cells[0][0].velHigh, 0,
                          "the bar-2 kick must be ignored — only the first populated bar imports");
            for (int v = 0; v < GrooveTable::kNumVoices; ++v)
                expectEquals ((int) t.cells[v][6].velHigh, 0,
                              "the cowbell (unmapped GM note) must not land anywhere");

            mid.deleteFile();
        }

        beginTest ("unreadable / non-MIDI files are rejected without touching the grid");
        {
            AdaptiveDrummerProcessor proc;
            proc.setGridCell (0, 0, true, 100);

            auto notMidi = juce::File::createTempFile ("adk_not_midi.mid");
            notMidi.replaceWithText ("definitely not a standard midi file");
            expect (! proc.importMidiGroove (notMidi), "garbage bytes must be rejected");
            expect (! proc.importMidiGroove (juce::File()), "a nonexistent file must be rejected");

            expect (proc.hasCustomGrid() && proc.getGridTable().cells[0][0].velHigh == 100,
                    "a failed MIDI import must leave the existing drawing untouched");
            notMidi.deleteFile();
        }
    }

private:
    // Somewhere above any version we will plausibly ever ship.
    static int kFutureVersion() { return 999; }

    struct TestNote { int channel, note, vel; double tick; };

    // Writes a one-track Standard MIDI File (tick-based, `tpqn` ticks per
    // quarter note) to a temp file — the makeTempKitWithKick idiom, for MIDI.
    juce::File writeTempMidi (int tpqn, std::initializer_list<TestNote> notes)
    {
        juce::MidiFile mf;
        mf.setTicksPerQuarterNote (tpqn);

        juce::MidiMessageSequence seq;
        for (const auto& n : notes)
        {
            seq.addEvent (juce::MidiMessage::noteOn  (n.channel, n.note, (juce::uint8) n.vel), n.tick);
            seq.addEvent (juce::MidiMessage::noteOff (n.channel, n.note), n.tick + 30.0);
        }
        seq.updateMatchedPairs();
        mf.addTrack (seq);

        auto file = juce::File::createTempFile ("adk_groove.mid");
        {
            juce::FileOutputStream os (file);
            if (os.openedOk())
                mf.writeTo (os);
        }
        return file;
    }
};

static PresetIOTest presetIOTest;
