#pragma once
#include <JuceHeader.h>

/**
 * XYPad — a draggable 2-D control bound directly to two host parameters via
 * juce::ParameterAttachment (the low-level primitive that
 * AudioProcessorValueTreeState::SliderAttachment itself wraps), so host
 * automation recording/playback works exactly as it would for a stock
 * JUCE control.
 *
 * Axis convention is the caller's choice, not hard-wired: xParameter maps
 * left(0)->right(1); yParameter maps bottom(0)->top(1) (inverted from pixel
 * Y, which grows downward).
 *
 * Follow/host-automation display contract: while disabled (setEnabled(false))
 * the pad ignores mouse input and paints a muted dot instead of an accent
 * one. The owner should call setDisplayPosition() on a timer to show a live
 * value that is NOT being written to the parameters -- mirroring the pattern
 * the sliders this replaces used (Slider::setValue(v, dontSendNotification)).
 */
class XYPad : public juce::Component
{
public:
    XYPad (juce::RangedAudioParameter& xParameter,
           juce::RangedAudioParameter& yParameter,
           juce::String xAxisLabel,
           juce::String yAxisLabel);
    ~XYPad() override;

    void paint (juce::Graphics&) override;

    void mouseDown        (const juce::MouseEvent&) override;
    void mouseDrag         (const juce::MouseEvent&) override;
    void mouseUp            (const juce::MouseEvent&) override;
    void mouseDoubleClick   (const juce::MouseEvent&) override;   // reset both axes to default

    /** Shows a live position without touching the bound parameters -- used
        while Follow is on. Both values are already-normalised [0,1]
        pad-space fractions (not the parameters' denormalised range). */
    void setDisplayPosition (float xValue01, float yValue01);

private:
    juce::Rectangle<float> getPadArea() const;
    void moveToMouseEvent (const juce::MouseEvent&);   // shared by mouseDown/mouseDrag

    juce::RangedAudioParameter& xParam;
    juce::RangedAudioParameter& yParam;
    juce::String xLabel, yLabel;

    juce::ParameterAttachment xAttachment;
    juce::ParameterAttachment yAttachment;

    float displayX01 { 0.5f };   // normalised [0,1]; what paint() actually draws
    float displayY01 { 0.5f };
    bool  gestureActive { false };   // true from a handled mouseDown until mouseUp,
                                      // independent of isEnabled() so a gesture that
                                      // began while enabled always ends cleanly even
                                      // if Follow toggles on mid-drag

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (XYPad)
};
