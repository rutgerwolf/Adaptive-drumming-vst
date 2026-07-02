#include <JuceHeader.h>
#include "PluginProcessor.h"

#include <cmath>

/**
 * Processor-level tests for AdaptiveDrummerProcessor — a CTest net that sits
 * below pluginval (Roadmap 2D, Step 2).
 *
 * processBlock() is always called from this test thread, never the message
 * thread. createEditor() is never invoked — these tests are headless; the
 * ScopedJuceInitialiser_GUI below only spins up the MessageManager that the
 * APVTS machinery needs.
 */
class AdaptiveDrummerProcessorTest : public juce::UnitTest
{
public:
    AdaptiveDrummerProcessorTest() : juce::UnitTest ("AdaptiveDrummerProcessor", "processor") {}

    void runTest() override
    {
        juce::ScopedJuceInitialiser_GUI guiInit;

        auto makeLayout = [] (juce::AudioChannelSet in, juce::AudioChannelSet out)
        {
            juce::AudioProcessor::BusesLayout layout;
            layout.inputBuses.add (in);
            layout.outputBuses.add (out);
            return layout;
        };

        beginTest ("bus layouts — accepted configurations");
        {
            AdaptiveDrummerProcessor proc;

            expect (proc.checkBusesLayoutSupported (
                        makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo())),
                    "stereo in / stereo out must be supported");

            expect (proc.checkBusesLayoutSupported (
                        makeLayout (juce::AudioChannelSet::mono(), juce::AudioChannelSet::mono())),
                    "mono in / mono out must be supported");

            expect (proc.checkBusesLayoutSupported (
                        makeLayout (juce::AudioChannelSet::disabled(), juce::AudioChannelSet::stereo())),
                    "disabled in / stereo out must be supported");
        }

        beginTest ("bus layouts — rejected configurations");
        {
            AdaptiveDrummerProcessor proc;

            expect (! proc.checkBusesLayoutSupported (
                        makeLayout (juce::AudioChannelSet::mono(), juce::AudioChannelSet::stereo())),
                    "mono in / stereo out must be rejected");

            expect (! proc.checkBusesLayoutSupported (
                        makeLayout (juce::AudioChannelSet::disabled(), juce::AudioChannelSet::quadraphonic())),
                    "disabled in / quad out must be rejected");
        }

        beginTest ("processBlock renders finite, audible audio while playing");
        {
            AdaptiveDrummerProcessor proc;

            const double sampleRate = 44100.0;
            const int    blockSize  = 512;

            proc.setPlayConfigDetails (2, 2, sampleRate, blockSize);
            proc.prepareToPlay (sampleRate, blockSize);

            proc.apvts.getParameter ("play")->setValueNotifyingHost (1.0f);

            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;

            bool   allFinite            = true;
            double accumulatedMagnitude = 0.0;

            for (int b = 0; b < 64; ++b)
            {
                buffer.clear();
                proc.processBlock (buffer, midi);

                // Deliberately not using buffer.getMagnitude()/getRMSLevel() here:
                // the processor writes through a getBusBuffer() alias of `buffer`,
                // which correctly clears *that* alias's cached hasBeenCleared()
                // flag, but this outer `buffer` object's own flag (set by the
                // clear() call above) is a separate bool that stays stale even
                // though both objects share the same underlying sample memory.
                // getMagnitude() trusts that stale flag and would wrongly report
                // silence, so scan the raw samples directly instead.
                for (int ch = 0; ch < buffer.getNumChannels() && allFinite; ++ch)
                {
                    const float* d = buffer.getReadPointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        if (! std::isfinite (d[i]))
                        {
                            allFinite = false;
                            break;
                        }

                        accumulatedMagnitude += (double) std::abs (d[i]);
                    }
                }
            }

            expect (allFinite, "every rendered sample must be finite");
            expectGreaterThan (accumulatedMagnitude, 0.0,
                                "default Synth source must be audible while playing, even with no samples loaded");

            proc.releaseResources();
        }

        beginTest ("state round-trip preserves style, bpm and volume");
        {
            AdaptiveDrummerProcessor proc;

            auto* styleParam  = proc.apvts.getParameter ("style");
            auto* bpmParam    = proc.apvts.getParameter ("bpm");
            auto* volumeParam = proc.apvts.getParameter ("volume");

            // Distinct, non-default values (defaults: style=Rock(0), bpm=120, volume=0.8).
            styleParam->setValueNotifyingHost  (styleParam->convertTo0to1  (1.0f));    // Jazz
            bpmParam->setValueNotifyingHost    (bpmParam->convertTo0to1    (150.0f));
            volumeParam->setValueNotifyingHost (volumeParam->convertTo0to1 (0.35f));

            juce::MemoryBlock savedState;
            proc.getStateInformation (savedState);

            AdaptiveDrummerProcessor fresh;
            fresh.setStateInformation (savedState.getData(), (int) savedState.getSize());

            expectEquals ((int) *fresh.apvts.getRawParameterValue ("style"), 1,
                          "style must survive the round-trip as Jazz");
            expectWithinAbsoluteError ((double) *fresh.apvts.getRawParameterValue ("bpm"), 150.0, 0.01,
                                       "bpm must survive the round-trip");
            expectWithinAbsoluteError ((double) *fresh.apvts.getRawParameterValue ("volume"), 0.35, 0.001,
                                       "volume must survive the round-trip");
        }
    }
};

static AdaptiveDrummerProcessorTest adaptiveDrummerProcessorTest;
