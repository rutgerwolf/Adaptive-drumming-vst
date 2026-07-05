#include "PresetIO.h"

#include <cmath>
#include <vector>

namespace
{
    // Style names in DrumPattern::Style order. Serialized as strings, not bare
    // indices: more robust/readable in a shared file, and insulated against
    // enum-value changes later.
    const char* const kStyleNames[] = { "Rock", "Jazz", "Electronic" };
    constexpr int kNumStyles = 3;

    juce::String styleToString (DrumPattern::Style s)
    {
        const int i = static_cast<int> (s);
        return (i >= 0 && i < kNumStyles) ? kStyleNames[i] : kStyleNames[0];
    }

    bool styleFromString (const juce::String& name, DrumPattern::Style& out)
    {
        for (int i = 0; i < kNumStyles; ++i)
            if (name == kStyleNames[i])
            {
                out = static_cast<DrumPattern::Style> (i);
                return true;
            }
        return false;
    }

    uint8_t clampToByte (int value, int lo, int hi)
    {
        return static_cast<uint8_t> (juce::jlimit (lo, hi, value));
    }
}

// ── JSON preset format (Part A) ───────────────────────────────────────────────

juce::String PresetIO::toJson (const Preset& p)
{
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty ("presetFormat", kPresetFormatVersion);
    root->setProperty ("name",         p.name);
    root->setProperty ("style",        styleToString (p.style));
    root->setProperty ("intensity",    p.intensity);
    root->setProperty ("complexity",   p.complexity);
    // int64, not int: the seed is a full uint32 and must stay positive and
    // human-readable in the file.
    root->setProperty ("grooveSeed",   static_cast<juce::int64> (p.grooveSeed));
    root->setProperty ("muteMask",     static_cast<int> (p.muteMask));

    if (p.hasCustomGrid)
    {
        juce::DynamicObject::Ptr grid = new juce::DynamicObject();
        grid->setProperty ("swing", static_cast<int> (p.customGrid.swing));

        // 6 voices × 16 steps, every StepCell field named explicitly —
        // verbose, but genuinely readable/debuggable, which is the point of a
        // user-facing exchange format. (styleSalt deliberately omitted.)
        juce::Array<juce::var> voices;
        for (int v = 0; v < GrooveTable::kNumVoices; ++v)
        {
            juce::Array<juce::var> row;
            for (int s = 0; s < GrooveTable::kNumSteps; ++s)
            {
                const StepCell& c = p.customGrid.cells[v][s];
                juce::DynamicObject::Ptr cell = new juce::DynamicObject();
                cell->setProperty ("velLow",              static_cast<int> (c.velLow));
                cell->setProperty ("velHigh",             static_cast<int> (c.velHigh));
                cell->setProperty ("complexityThreshold", static_cast<int> (c.complexityThreshold));
                cell->setProperty ("probability",         static_cast<int> (c.probability));
                cell->setProperty ("velRandRange",        static_cast<int> (c.velRandRange));
                cell->setProperty ("flags",               static_cast<int> (c.flags));
                row.add (juce::var (cell.get()));
            }
            voices.add (juce::var (row));
        }
        grid->setProperty ("cells", juce::var (voices));
        root->setProperty ("customGrid", juce::var (grid.get()));
    }
    else
    {
        root->setProperty ("customGrid", juce::var());   // serializes as null
    }

    return juce::JSON::toString (juce::var (root.get()));
}

bool PresetIO::fromJson (const juce::String& jsonText, Preset& out)
{
    const juce::var root = juce::JSON::parse (jsonText);
    if (root.getDynamicObject() == nullptr)
        return false;   // not JSON, or not an object

    // Unknown/future format: reject outright rather than guess — the same
    // posture as the internal gridVersion check.
    if (static_cast<int> (root.getProperty ("presetFormat", 0)) != kPresetFormatVersion)
        return false;

    // Build into a local so `out` is untouched unless everything validates —
    // a bad file must never partially apply.
    Preset p;
    p.name = root.getProperty ("name", juce::String()).toString();

    if (! styleFromString (root.getProperty ("style", juce::var()).toString(), p.style))
        return false;

    p.intensity  = juce::jlimit (0.0f, 1.0f,
                       static_cast<float> (static_cast<double> (root.getProperty ("intensity",  0.55))));
    p.complexity = juce::jlimit (0.0f, 1.0f,
                       static_cast<float> (static_cast<double> (root.getProperty ("complexity", 0.55))));
    p.grooveSeed = static_cast<uint32_t> (static_cast<juce::int64> (
                       root.getProperty ("grooveSeed", static_cast<juce::int64> (DrumPattern::kDefaultGrooveSeed))));
    p.muteMask   = static_cast<uint8_t> (static_cast<int> (root.getProperty ("muteMask", 0)) & 0x3F);

    const juce::var grid = root.getProperty ("customGrid", juce::var());
    if (! grid.isVoid())
    {
        if (grid.getDynamicObject() == nullptr)
            return false;   // present but neither null nor an object — garbage

        p.customGrid       = GrooveTable {};
        p.customGrid.swing = clampToByte (static_cast<int> (grid.getProperty ("swing", 0)), 0, 255);
        // styleSalt stays 0 here: never serialized (internal constant); the
        // processor re-salts from the preset's style on apply.

        const auto* voicesArr = grid.getProperty ("cells", juce::var()).getArray();
        if (voicesArr == nullptr || voicesArr->size() != GrooveTable::kNumVoices)
            return false;

        for (int v = 0; v < GrooveTable::kNumVoices; ++v)
        {
            const auto* row = (*voicesArr)[v].getArray();
            if (row == nullptr || row->size() != GrooveTable::kNumSteps)
                return false;

            for (int s = 0; s < GrooveTable::kNumSteps; ++s)
            {
                const juce::var& cv = (*row)[s];
                if (! cv.isObject())
                    return false;

                StepCell& c            = p.customGrid.cells[v][s];
                c.velLow               = clampToByte (static_cast<int> (cv.getProperty ("velLow",  0)), 0, 127);
                c.velHigh              = clampToByte (static_cast<int> (cv.getProperty ("velHigh", 0)), 0, 127);
                c.complexityThreshold  = clampToByte (static_cast<int> (cv.getProperty ("complexityThreshold", 0)), 0, 255);
                c.probability          = clampToByte (static_cast<int> (cv.getProperty ("probability", 255)), 0, 255);
                c.velRandRange         = static_cast<int8_t> (juce::jlimit (-128, 127,
                                             static_cast<int> (cv.getProperty ("velRandRange", 0))));
                c.flags                = clampToByte (static_cast<int> (cv.getProperty ("flags", 0)), 0, 255);
            }
        }
        p.hasCustomGrid = true;
    }

    out = std::move (p);
    return true;
}

// ── MIDI groove import (Part B) ───────────────────────────────────────────────

// The full GM1 percussion key map (35..81), grouped into our six voice
// families. Built from the GM1 spec, not just our own canonical six notes, so
// real-world groove packs (Groove Monkee, Toontrack, …) import sensibly.
int PresetIO::voiceForGmNote (int midiNote) noexcept
{
    switch (midiNote)
    {
        // ── Kick family ──
        case 35:   // Acoustic Bass Drum
        case 36:   // Bass Drum 1 (our canonical kick)
            return 0;

        // ── Snare family (side stick / clap play the snare role) ──
        case 37:   // Side Stick
        case 38:   // Acoustic Snare (canonical)
        case 39:   // Hand Clap
        case 40:   // Electric Snare
            return 1;

        // ── Hi-hat family (+ hand-percussion timekeepers) ──
        case 42:   // Closed Hi-Hat (canonical)
        case 44:   // Pedal Hi-Hat
        case 46:   // Open Hi-Hat
        case 54:   // Tambourine
        case 69:   // Cabasa
        case 70:   // Maracas
            return 2;

        // ── Crash family ──
        case 49:   // Crash Cymbal 1 (canonical)
        case 52:   // Chinese Cymbal
        case 55:   // Splash Cymbal
        case 57:   // Crash Cymbal 2
            return 3;

        // ── Ride family ──
        case 51:   // Ride Cymbal 1 (canonical)
        case 53:   // Ride Bell
        case 59:   // Ride Cymbal 2
            return 4;

        // ── Tom family (all six GM toms; bongos/congas/timbales in the tom role) ──
        case 41:   // Low Floor Tom
        case 43:   // High Floor Tom
        case 45:   // Low Tom (canonical)
        case 47:   // Low-Mid Tom
        case 48:   // Hi-Mid Tom
        case 50:   // High Tom
        case 60:   // Hi Bongo
        case 61:   // Low Bongo
        case 62:   // Mute Hi Conga
        case 63:   // Open Hi Conga
        case 64:   // Low Conga
        case 65:   // High Timbale
        case 66:   // Low Timbale
            return 5;

        // Cowbell (56), vibraslap (58), agogos, claves, woodblocks, triangle,
        // whistles, guiro, cuica, shakers beyond the above: no sensible home
        // in a 6-voice kit — ignored rather than mis-voiced.
        default:
            return -1;
    }
}

bool PresetIO::grooveTableFromMidiFile (const juce::File& midiFile, GrooveTable& out)
{
    juce::FileInputStream in (midiFile);
    if (! in.openedOk())
        return false;

    juce::MidiFile mf;
    if (! mf.readFrom (in))
        return false;

    const int tpqn = mf.getTimeFormat();
    if (tpqn <= 0)
        return false;   // SMPTE-timed: not bar/beat material (see the header)

    // Bar length: the first time-signature meta-event if any, else 4/4.
    double beatsPerBar = 4.0;   // in quarter notes
    {
        juce::MidiMessageSequence sigs;
        mf.findAllTimeSigEvents (sigs);
        if (sigs.getNumEvents() > 0)
        {
            int num = 4, den = 4;
            sigs.getEventPointer (0)->message.getTimeSignatureInfo (num, den);
            if (num > 0 && den > 0)
                beatsPerBar = num * (4.0 / den);
        }
    }

    const double stepTicks = (tpqn * beatsPerBar) / GrooveTable::kNumSteps;
    if (stepTicks <= 0.0)
        return false;

    // Channel heuristic: if ANY note-on sits on channel 10 (the GM percussion
    // convention), trust it and ignore every other channel (they're melodic
    // parts); with no channel-10 notes at all, scan every channel — some
    // hobbyist/free packs put drums on channel 1.
    bool anyChannel10 = false;
    for (int t = 0; t < mf.getNumTracks() && ! anyChannel10; ++t)
    {
        const auto* track = mf.getTrack (t);
        for (int e = 0; e < track->getNumEvents(); ++e)
        {
            const auto& msg = track->getEventPointer (e)->message;
            if (msg.isNoteOn() && msg.getChannel() == 10)
            {
                anyChannel10 = true;
                break;
            }
        }
    }

    struct Hit { int voice, step, vel; juce::int64 bar; };
    std::vector<Hit> hits;

    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        const auto* track = mf.getTrack (t);
        for (int e = 0; e < track->getNumEvents(); ++e)
        {
            const auto& msg = track->getEventPointer (e)->message;
            if (! msg.isNoteOn())
                continue;
            if (anyChannel10 && msg.getChannel() != 10)
                continue;

            const int voice = voiceForGmNote (msg.getNoteNumber());
            if (voice < 0)
                continue;

            // Quantize globally to the nearest 16th, THEN split into
            // (bar, step): a humanized hit pushed slightly before a barline
            // still rounds onto that bar's downbeat instead of being lost to
            // the previous bar.
            const auto globalStep = static_cast<juce::int64> (
                std::llround (msg.getTimeStamp() / stepTicks));
            hits.push_back ({ voice,
                              static_cast<int> (globalStep % GrooveTable::kNumSteps),
                              static_cast<int> (msg.getVelocity()),
                              globalStep / GrooveTable::kNumSteps });
        }
    }

    if (hits.empty())
        return false;   // readable MIDI, but nothing mappable to percussion

    // Import only the first bar that contains any mapped note (simplest,
    // most predictable — no cross-bar merging/majority-voting).
    juce::int64 firstBar = hits.front().bar;
    for (const auto& h : hits)
        firstBar = juce::jmin (firstBar, h.bar);

    out = GrooveTable {};   // all cells silent; swing 0, styleSalt 0
    for (const auto& h : hits)
    {
        if (h.bar != firstBar)
            continue;

        // Hand-drawn-cell convention (see setGridCell): deterministic,
        // always-eligible, exactly the played velocity. Collisions keep the
        // highest velocity so accents survive collapsed flams/doubles.
        const auto vel = clampToByte (h.vel, 1, 127);
        StepCell& cell = out.cells[h.voice][h.step];
        if (vel > cell.velHigh)
            cell = StepCell { vel, vel, 0, 255, 0, 0 };
    }

    return true;
}
