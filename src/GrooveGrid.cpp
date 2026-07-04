#include "GrooveGrid.h"

#include <cstring>

// Same palette PluginEditor.cpp uses, duplicated per the XYPad.cpp precedent
// (still no shared UI-constants header; a third user is where one would start
// to pay off, but that refactor is out of this feature's scope).
static const juce::Colour kPanel  { 0xff16213e };
static const juce::Colour kMuted  { 0xff888899 };
static const juce::Colour kAccent { 0xff6c9bd1 };

static const char* const kVoiceNames[GrooveTable::kNumVoices]
    = { "Kick", "Snare", "HiHat", "Crash", "Ride", "Tom" };

GrooveGrid::GrooveGrid() = default;

void GrooveGrid::setTable (const GrooveTable& newTable, uint8_t newMuteMask)
{
    // Byte compare: the 10 Hz poll almost always pushes an identical table
    // (our own edits are applied locally first and echoed back unchanged), so
    // skip the repaint unless something genuinely moved (a state restore, a
    // style pick, host automation, a mute toggle).
    if (std::memcmp (&table, &newTable, sizeof (GrooveTable)) == 0 && muteMask == newMuteMask)
        return;

    table    = newTable;
    muteMask = newMuteMask;
    repaint();
}

juce::Rectangle<float> GrooveGrid::gridArea() const
{
    return getLocalBounds().toFloat().withTrimmedLeft (kGutterWidth);
}

juce::Rectangle<float> GrooveGrid::cellArea (int voice, int step) const
{
    const auto area  = gridArea();
    const float cellW = area.getWidth()  / GrooveTable::kNumSteps;
    const float cellH = area.getHeight() / GrooveTable::kNumVoices;
    return { area.getX() + step  * cellW,
             area.getY() + voice * cellH,
             cellW, cellH };
}

int GrooveGrid::stepForX (float x) const
{
    const auto area = gridArea();
    const int step  = (int) std::floor ((x - area.getX()) / area.getWidth()
                                        * GrooveTable::kNumSteps);
    return juce::jlimit (0, GrooveTable::kNumSteps - 1, step);
}

bool GrooveGrid::hitTestCell (juce::Point<float> pos, int& voice, int& step) const
{
    const auto area = gridArea();
    if (! area.contains (pos))
        return false;

    step  = stepForX (pos.x);
    voice = juce::jlimit (0, GrooveTable::kNumVoices - 1,
                          (int) std::floor ((pos.y - area.getY()) / area.getHeight()
                                            * GrooveTable::kNumVoices));
    return true;
}

void GrooveGrid::paint (juce::Graphics& g)
{
    g.setFont (juce::Font (9.0f));

    for (int v = 0; v < GrooveTable::kNumVoices; ++v)
    {
        const bool rowMuted = (muteMask & (1u << v)) != 0;
        // Dim everything on a muted row — read-only, composing mutes win.
        const float rowAlpha = rowMuted ? 0.30f : 1.0f;

        // Voice label in the left gutter.
        const auto labelArea = cellArea (v, 0);
        g.setColour (kMuted.withMultipliedAlpha (rowAlpha));
        g.drawText (kVoiceNames[v], 0, (int) labelArea.getY(),
                    (int) kGutterWidth - 4, (int) labelArea.getHeight(),
                    juce::Justification::centredRight);

        for (int s = 0; s < GrooveTable::kNumSteps; ++s)
        {
            const auto cell = cellArea (v, s).reduced (1.0f);

            // Beat-group shading: beats 2 and 4 (steps 4-7 / 12-15) slightly
            // lighter, so the bar reads at a glance.
            const bool altBeat = ((s / 4) % 2) != 0;
            g.setColour ((altBeat ? kPanel.brighter (0.25f) : kPanel)
                             .withMultipliedAlpha (rowAlpha));
            g.fillRect (cell);

            const StepCell& c = table.cells[v][s];
            if (c.velHigh > 0)
            {
                // Fill height ∝ velHigh; conditional (style-inherited,
                // probability/threshold-gated) cells translucent, guaranteed
                // cells (backbone + hand-drawn) solid.
                const bool guaranteed = c.probability == 255 && c.complexityThreshold == 0;
                const float h = cell.getHeight() * (float) c.velHigh / 127.0f;
                g.setColour (kAccent.withMultipliedAlpha ((guaranteed ? 1.0f : 0.45f) * rowAlpha));
                g.fillRect (cell.withTop (cell.getBottom() - h));
            }
        }
    }

    g.setColour (kMuted.withAlpha (0.6f));
    g.drawRect (gridArea(), 1.0f);
}

void GrooveGrid::editCell (int voice, int step, bool on, int velocity)
{
    // Local copy uses the exact StepCell conventions the processor applies in
    // setGridCell(), so the next poll's echo is byte-identical (no flicker).
    if (on)
    {
        const auto vel = (uint8_t) juce::jlimit (1, 127, velocity);
        table.cells[voice][step] = StepCell { vel, vel, 0, 255, 0, 0 };
    }
    else
    {
        table.cells[voice][step] = StepCell {};
    }

    repaint();

    if (onCellEdit != nullptr)
        onCellEdit (voice, step, on, velocity);
}

void GrooveGrid::mouseDown (const juce::MouseEvent& e)
{
    int voice = 0, step = 0;
    if (! isEnabled() || ! hitTestCell (e.position, voice, step))
        return;

    if ((muteMask & (1u << voice)) != 0)
        return;   // muted rows are read-only (see the class comment)

    gestureActive          = true;
    exceededDragThreshold  = false;
    gestureVoice           = voice;
    pressStep              = step;
    lastDragStep           = step;
    pressPos               = e.position;
    std::memset (touchedInGesture, 0, sizeof (touchedInGesture));

    const StepCell& c = table.cells[voice][step];
    if (c.velHigh > 0)
    {
        // Filled cell: adjust mode (velocity, relative to the current value);
        // a plain click resolves to "remove" at mouseUp.
        paintMode    = false;
        baseVelocity = (int) c.velHigh;
    }
    else
    {
        // Empty cell: paint mode — the hit lands immediately.
        paintMode    = true;
        baseVelocity = kDefaultVelocity;
        editCell (voice, step, true, kDefaultVelocity);
    }

    gestureVelocity = baseVelocity;
    touchedInGesture[step] = true;
}

void GrooveGrid::mouseDrag (const juce::MouseEvent& e)
{
    if (! gestureActive)
        return;

    if (e.position.getDistanceFrom (pressPos) > kDragThreshold)
        exceededDragThreshold = true;

    // Vertical drag = velocity (up = louder), relative to the mouse-down
    // value. Row-locked (gestureVoice fixed), so vertical movement is free to
    // mean velocity without hopping rows.
    const int newVelocity = juce::jlimit (1, 127,
        baseVelocity + juce::roundToInt ((pressPos.y - e.position.y) * kVelPerPixel));

    // Horizontal drag = extend the gesture along the row. X is clamped so a
    // sweep that overshoots the edges still reaches the first/last step. A
    // single mouseDrag callback can skip several steps when the mouse moves
    // fast (JUCE doesn't guarantee one callback per pixel), so in paint mode
    // the whole [lastStep, step] span is filled in rather than just the
    // endpoint — otherwise a quick sideways flick would punch holes in the
    // painted run instead of drawing a continuous one.
    const int step = stepForX (e.position.x);
    const int lo = juce::jmin (lastDragStep, step);
    const int hi = juce::jmax (lastDragStep, step);
    for (int s = lo; s <= hi; ++s)
    {
        if (touchedInGesture[s])
            continue;

        if (paintMode)
            touchedInGesture[s] = true;                   // paint: add the cell
        else if (table.cells[gestureVoice][s].velHigh > 0)
            touchedInGesture[s] = true;                   // adjust: level existing cells only
    }
    lastDragStep = step;

    if (! exceededDragThreshold)
        return;

    // Live-update every cell this gesture owns to the current velocity.
    gestureVelocity = newVelocity;
    for (int s = 0; s < GrooveTable::kNumSteps; ++s)
        if (touchedInGesture[s]
            && table.cells[gestureVoice][s].velHigh != (uint8_t) gestureVelocity)
            editCell (gestureVoice, s, true, gestureVelocity);
}

void GrooveGrid::mouseUp (const juce::MouseEvent&)
{
    if (! gestureActive)
        return;

    gestureActive = false;

    // A plain click (no real drag) on a filled cell removes it. Paint-mode
    // clicks already placed their hit at mouseDown.
    if (! paintMode && ! exceededDragThreshold)
        editCell (gestureVoice, pressStep, false, 0);
}
