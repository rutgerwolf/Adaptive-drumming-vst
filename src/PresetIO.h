#pragma once
#include <JuceHeader.h>
#include "drummer/DrumPattern.h"

/**
 * PresetIO — the user-facing, shareable preset file format (Part A) and the
 * Standard-MIDI-File groove importer (Part B).
 *
 * Two deliberately separate concepts, matching how every comparable drum tool
 * splits this:
 *
 *   - .adpreset: an app-native, versioned, HUMAN-READABLE JSON file carrying a
 *     full "sound" — style, the two groove axes, mute mask, groove seed, and
 *     (optionally) the complete hand-drawn custom grid, every StepCell field
 *     named explicitly. This is NOT the internal gridData/base64 state blob in
 *     PluginProcessor.cpp (that one is a session-persistence detail); this one
 *     is a file users open in a text editor, share, and debug.
 *
 *   - MIDI import: a lossy, universal on-ramp for the existing ecosystem of
 *     MIDI drum groove packs (channel-10 GM percussion convention). One bar of
 *     note-ons is quantized into the 6×16 GrooveTable and lands as a custom
 *     grid, fully editable like a hand-drawn pattern.
 *
 * styleSalt is deliberately NOT serialized in either direction: it is an
 * internal hash-decorrelation constant, meaningless outside this codebase —
 * importers leave it 0 and the processor re-salts from the preset's style.
 */
namespace PresetIO
{
    /** Bumped on any incompatible schema change; fromJson() rejects any other
        value (same defensive posture as the internal kGridFormatVersion). */
    constexpr int kPresetFormatVersion = 1;

    /** The user-facing preset file extension (plain-JSON content). An own
        extension rather than bare .json so the files are recognisable and
        double-click/file-filter friendly, while still opening in any text
        editor. */
    constexpr const char* kPresetFileExtension = ".adpreset";

    /** One preset's full payload. customGrid is only meaningful when
        hasCustomGrid is true; its styleSalt is never serialized (see above). */
    struct Preset
    {
        juce::String       name;
        DrumPattern::Style style      { DrumPattern::Style::Rock };
        float              intensity  { 0.55f };
        float              complexity { 0.55f };
        uint32_t           grooveSeed { DrumPattern::kDefaultGrooveSeed };
        uint8_t            muteMask   { 0 };
        bool               hasCustomGrid { false };
        GrooveTable        customGrid {};
    };

    /** Serializes a preset as the versioned, human-readable JSON document. */
    juce::String toJson (const Preset& preset);

    /** Parses + validates a preset JSON. Returns false (leaving `out`
        untouched) on: unparseable JSON, a missing/unknown presetFormat, an
        unknown style name, or a present-but-malformed customGrid (wrong
        voice/step counts, non-object cells). Numeric fields are clamped to
        their legal ranges rather than rejected. */
    bool fromJson (const juce::String& jsonText, Preset& out);

    /** GM1 percussion key (35..81) → DrumPattern voice index (0 Kick … 5 Tom),
        grouped by family so real-world packs map sensibly (e.g. 40 Electric
        Snare → Snare, 46 Open Hi-Hat → HiHat). Returns -1 for notes with no
        sensible 6-voice home (cowbell, claves, woodblocks, …). */
    int voiceForGmNote (int midiNote) noexcept;

    /** Reads a Standard MIDI File and quantizes its percussion note-ons into
        one GrooveTable bar (16 steps).

        Rules (each a documented simplification):
          - Only tick-based (ticks-per-quarter-note) files; SMPTE-timed files
            are rejected — groove packs are universally bar/beat material.
          - If any channel-10 note-on exists, only channel 10 is read (the GM
            percussion convention); otherwise ALL channels are scanned, since
            some hobbyist/free packs ignore channel 10.
          - The first time-signature meta-event (if any) sets the bar length;
            otherwise 4/4 is assumed. Mid-file signature changes are ignored.
          - Each note-on is quantized to the nearest global 16th step; only
            the FIRST bar containing any mapped note is imported (predictable,
            no cross-bar merging). Quantizing globally first means a humanized
            hit pushed slightly before a barline still lands on that bar's
            downbeat instead of being lost to the previous bar.
          - Collisions (two notes quantizing to one voice+step) keep the
            highest velocity, so accents survive collapsed flams/doubles.
          - Every imported hit uses the hand-drawn-cell convention:
            { vel, vel, threshold 0, probability 255, rand 0, flags 0 }.
        The output table is otherwise zeroed (swing 0 — the timing was read
        from real positions and quantized straight; styleSalt 0 — see above).

        Returns false (leaving `out` untouched) if the file can't be read or
        contains no mappable percussion note-ons. */
    bool grooveTableFromMidiFile (const juce::File& midiFile, GrooveTable& out);
}
