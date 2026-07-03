#include "XYPad.h"

// Same 4 colours PluginEditor.cpp uses, duplicated here (no shared UI-constants
// header exists yet in this codebase; 4 lines is the smallest, most consistent
// choice over introducing one for a single second user).
static const juce::Colour kPanel  { 0xff16213e };
static const juce::Colour kText   { 0xffe0e0e0 };
static const juce::Colour kMuted  { 0xff888899 };
static const juce::Colour kAccent { 0xff6c9bd1 };

XYPad::XYPad (juce::RangedAudioParameter& xParameter,
             juce::RangedAudioParameter& yParameter,
             juce::String xAxisLabel,
             juce::String yAxisLabel)
    : xParam (xParameter), yParam (yParameter),
      xLabel (std::move (xAxisLabel)), yLabel (std::move (yAxisLabel)),
      xAttachment (xParameter, [this] (float denormalised)
                   {
                       displayX01 = xParam.convertTo0to1 (denormalised);
                       repaint();
                   }),
      yAttachment (yParameter, [this] (float denormalised)
                   {
                       displayY01 = yParam.convertTo0to1 (denormalised);
                       repaint();
                   })
{
    // Required by ParameterAttachment's contract: makes the pad reflect the
    // parameters' actual current value immediately (e.g. a restored session
    // that isn't at the default), rather than starting at the {0.5,0.5} member
    // initialisers until the first external change.
    xAttachment.sendInitialUpdate();
    yAttachment.sendInitialUpdate();
}

XYPad::~XYPad() = default;

juce::Rectangle<float> XYPad::getPadArea() const
{
    return getLocalBounds().toFloat().reduced (1.0f);
}

void XYPad::paint (juce::Graphics& g)
{
    const auto area = getPadArea();

    g.setColour (kPanel);
    g.fillRect (area);

    // Faint centre crosshair.
    g.setColour (kMuted.withAlpha (0.25f));
    g.drawLine (area.getCentreX(), area.getY(), area.getCentreX(), area.getBottom());
    g.drawLine (area.getX(), area.getCentreY(), area.getRight(), area.getCentreY());

    g.setColour (kMuted);
    g.drawRect (area, 1.0f);

    // Axis captions, tucked inside the corners so the pad stays fully
    // self-contained (no extra layout coordination needed from the owner).
    g.setFont (juce::Font (9.0f));
    g.setColour (kMuted);
    g.drawText (yLabel, (int) area.getX() + 4, (int) area.getY() + 3, 100, 12,
               juce::Justification::left);
    g.drawText (xLabel, (int) area.getX(), (int) area.getBottom() - 15,
               (int) area.getWidth() - 4, 12, juce::Justification::right);

    // The draggable dot: dimmed to kMuted while disabled (Follow is live).
    const float dotX = area.getX() + displayX01 * area.getWidth();
    const float dotY = area.getY() + (1.0f - displayY01) * area.getHeight();   // Y is inverted: 1.0 draws at the top
    constexpr float dotRadius = 6.0f;

    g.setColour (isEnabled() ? kAccent : kMuted);
    g.fillEllipse (dotX - dotRadius, dotY - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);
    g.setColour (kText);
    g.drawEllipse (dotX - dotRadius, dotY - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f, 1.0f);
}

void XYPad::moveToMouseEvent (const juce::MouseEvent& e)
{
    const auto area = getPadArea();
    if (area.isEmpty())
        return;

    const float x01 = juce::jlimit (0.0f, 1.0f, (float) (e.position.x - area.getX()) / area.getWidth());
    const float y01 = juce::jlimit (0.0f, 1.0f, 1.0f - (float) (e.position.y - area.getY()) / area.getHeight());

    // Update the display immediately (don't wait for the attachment's
    // message-thread round-trip) so dragging feels 1:1 responsive; the
    // attachment calls below will harmlessly re-set the same value shortly.
    displayX01 = x01;
    displayY01 = y01;

    xAttachment.setValueAsPartOfGesture (xParam.convertFrom0to1 (x01));
    yAttachment.setValueAsPartOfGesture (yParam.convertFrom0to1 (y01));

    repaint();
}

void XYPad::mouseDown (const juce::MouseEvent& e)
{
    if (! isEnabled())
        return;

    gestureActive = true;
    xAttachment.beginGesture();
    yAttachment.beginGesture();
    moveToMouseEvent (e);
}

void XYPad::mouseDrag (const juce::MouseEvent& e)
{
    if (! gestureActive)
        return;

    moveToMouseEvent (e);
}

void XYPad::mouseUp (const juce::MouseEvent&)
{
    if (! gestureActive)
        return;

    gestureActive = false;
    xAttachment.endGesture();
    yAttachment.endGesture();
}

void XYPad::mouseDoubleClick (const juce::MouseEvent&)
{
    if (! isEnabled())
        return;

    // getDefaultValue() is already normalised (JUCE convention, confirmed via
    // AudioParameterFloat::getDefaultValue() == convertTo0to1(valueDefault));
    // convertFrom0to1(getDefaultValue()) is the exact idiom JUCE's own
    // SliderParameterAttachment uses to get the *denormalised* default for
    // setDoubleClickReturnValue().
    displayX01 = xParam.getDefaultValue();
    displayY01 = yParam.getDefaultValue();

    xAttachment.setValueAsCompleteGesture (xParam.convertFrom0to1 (xParam.getDefaultValue()));
    yAttachment.setValueAsCompleteGesture (yParam.convertFrom0to1 (yParam.getDefaultValue()));

    repaint();
}

void XYPad::setDisplayPosition (float xValue01, float yValue01)
{
    displayX01 = juce::jlimit (0.0f, 1.0f, xValue01);
    displayY01 = juce::jlimit (0.0f, 1.0f, yValue01);
    repaint();
}
