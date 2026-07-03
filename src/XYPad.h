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
 * While disabled (setEnabled(false)) the pad ignores mouse input and paints a
 * muted dot instead of an accent one -- ordinary juce::Component convention,
 * not tied to any one caller's feature.
 *
 * Follow overlay: the pad stays fully draggable while Follow is on (the grid
 * remains authoritative -- see DrumPattern's class comment), so Follow's
 * modulation is drawn as an overlay ring on top of the normal puck, not by
 * hijacking the puck's position or disabling the pad. See setFollowOverlay().
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

    /** Shows/hides the Follow overlay: a hollow ring at the puck's X (complexity
        is never modulated by Follow) but at intensityEff01's Y, connected to
        the actual puck by a thin line, pulsing with accent01. Purely visual --
        never touches the bound parameters. Call with active=false (the
        default) when Follow is off or has no guide to modulate with. */
    void setFollowOverlay (bool active, float intensityEff01, float accent01);

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

    bool  followOverlayActive { false };
    float followIntensity01   { 0.5f };   // Y position of the overlay ring
    float followAccent01      { 0.0f };   // 0..1, drives the ring's pulse

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (XYPad)
};
