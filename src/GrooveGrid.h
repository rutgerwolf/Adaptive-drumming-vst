#pragma once
#include <JuceHeader.h>
#include "drummer/DrumPattern.h"

/**
 * GrooveGrid — the user-drawable 6-voice × 16-step pattern grid (roadmap
 * Step 19's destination; the reason TableExchange exists).
 *
 * Purely a view/controller over a GrooveTable the owner supplies: the editor
 * polls the processor at 10 Hz and pushes the currently-intended table in via
 * setTable() (same poll pattern as the style/mute buttons); every user edit
 * goes back out through onCellEdit, which the editor forwards to
 * AdaptiveDrummerProcessor::setGridCell(). The component also mutates its
 * local copy immediately (with the exact same StepCell conventions the
 * processor applies), so painting is 1:1 responsive and the next poll is a
 * byte-identical no-op — mirroring XYPad's update-display-immediately idiom.
 *
 * Interaction (GarageBand/Logic-style step sequencer, row-locked gestures —
 * the voice row is fixed at mouse-down so horizontal drag paints steps and
 * vertical drag is free to mean velocity):
 *   - click an empty cell        → hit at the default velocity (100)
 *   - click a filled cell        → remove it
 *   - drag from an empty cell    → paint cells on across the row; vertical
 *                                  movement sets the gesture velocity, which
 *                                  live-updates every cell of this gesture
 *   - drag from a filled cell    → adjust velocity (relative, from the cell's
 *                                  current value); dragging across other
 *                                  filled cells levels them to the same value
 *                                  (empty cells are NOT added in this mode)
 * Muted rows (per the owner-supplied mask) are read-only and drawn dimmed —
 * the drawing under them survives, mutes compose non-destructively on top.
 *
 * Cell display: fill height ∝ velHigh (0..127). Cells inherited from a
 * built-in style that are conditional (probability < 255 or a nonzero
 * complexity threshold) draw translucent; guaranteed cells — the authored
 * backbone and every hand-drawn hit — draw solid.
 */
class GrooveGrid : public juce::Component
{
public:
    GrooveGrid();

    /** Fired for every cell the user changes: on=true with velocity 1..127,
        or on=false (velocity ignored). The owner routes this to
        AdaptiveDrummerProcessor::setGridCell(). */
    std::function<void (int voice, int step, bool on, int velocity)> onCellEdit;

    /** The table to display/edit plus the current mute mask (bit v = voice v
        muted). Cheap to call every poll tick: repaints only on a real change. */
    void setTable (const GrooveTable& newTable, uint8_t newMuteMask);

    void paint     (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> gridArea() const;                     // cells only (right of the label gutter)
    juce::Rectangle<float> cellArea (int voice, int step) const;
    bool hitTestCell (juce::Point<float> pos, int& voice, int& step) const;
    int  stepForX (float x) const;                                // clamped to [0, kNumSteps)
    void editCell (int voice, int step, bool on, int velocity);   // local copy + onCellEdit

    static constexpr int   kDefaultVelocity = 100;
    static constexpr float kGutterWidth     = 44.0f;   // voice-name labels
    static constexpr float kDragThreshold   = 3.0f;    // px: click vs. drag
    static constexpr float kVelPerPixel     = 0.75f;   // full 1..127 sweep over ~170 px

    GrooveTable table {};
    uint8_t     muteMask { 0 };

    // Gesture state (one mouse gesture = one row-locked edit).
    bool gestureActive   { false };
    bool paintMode       { false };   // started on an empty cell
    bool exceededDragThreshold { false };
    int  gestureVoice    { 0 };
    int  pressStep       { 0 };
    int  lastDragStep    { 0 };   // for filling in skipped steps on a fast paint-mode drag
    int  baseVelocity    { kDefaultVelocity };   // velocity at mouse-down
    int  gestureVelocity { kDefaultVelocity };   // baseVelocity ± vertical drag
    juce::Point<float> pressPos;
    bool touchedInGesture[GrooveTable::kNumSteps] {};   // cells this gesture owns

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrooveGrid)
};
