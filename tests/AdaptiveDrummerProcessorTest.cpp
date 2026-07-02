#include <JuceHeader.h>
#include "PluginProcessor.h"

#include <cmath>

/**
 * A host playhead that reports isPlaying=true but never supplies a ppq
 * position — simulates a real host quirk (Fable review Finding #6): some
 * hosts report one without the other, and treating "no ppq" as "not
 * playing" wrongly silenced playback.
 */
class NoPpqPlayHead : public juce::AudioPlayHead
{
public:
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (true);
        info.setBpm (130.0);
        // Deliberately no setPpqPosition() call.
        return info;
    }
};

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

        beginTest ("host isPlaying without a ppq position still produces audio (Finding #6)");
        {
            AdaptiveDrummerProcessor proc;
            NoPpqPlayHead            fakePlayHead;
            proc.setPlayHead (&fakePlayHead);

            const double sampleRate = 44100.0;
            const int    blockSize  = 512;

            proc.setPlayConfigDetails (2, 2, sampleRate, blockSize);
            proc.prepareToPlay (sampleRate, blockSize);
            // Note: the "play" parameter is deliberately left off — only the
            // host's isPlaying flag should drive playback here.

            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;

            bool   allFinite            = true;
            double accumulatedMagnitude = 0.0;

            for (int b = 0; b < 64; ++b)
            {
                buffer.clear();
                proc.processBlock (buffer, midi);

                for (int ch = 0; ch < buffer.getNumChannels() && allFinite; ++ch)
                {
                    const float* d = buffer.getReadPointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                    {
                        if (! std::isfinite (d[i])) { allFinite = false; break; }
                        accumulatedMagnitude += (double) std::abs (d[i]);
                    }
                }
            }

            expect (allFinite, "every rendered sample must be finite");
            expectGreaterThan (accumulatedMagnitude, 0.0,
                                "a host reporting isPlaying without ppq must still produce audio, "
                                "not be silently treated as stopped");

            proc.releaseResources();
        }

        beginTest ("state round-trip preserves style, bpm, volume, intensity and complexity");
        {
            AdaptiveDrummerProcessor proc;

            auto* styleParam      = proc.apvts.getParameter ("style");
            auto* bpmParam        = proc.apvts.getParameter ("bpm");
            auto* volumeParam     = proc.apvts.getParameter ("volume");
            auto* intensityParam  = proc.apvts.getParameter ("intensity");
            auto* complexityParam = proc.apvts.getParameter ("complexity");

            // Distinct, non-default values (defaults: style=Rock(0), bpm=120,
            // volume=0.8, intensity=complexity=0.55).
            styleParam->setValueNotifyingHost      (styleParam->convertTo0to1      (1.0f));    // Jazz
            bpmParam->setValueNotifyingHost        (bpmParam->convertTo0to1        (150.0f));
            volumeParam->setValueNotifyingHost     (volumeParam->convertTo0to1     (0.35f));
            intensityParam->setValueNotifyingHost  (intensityParam->convertTo0to1  (0.9f));
            complexityParam->setValueNotifyingHost (complexityParam->convertTo0to1 (0.1f));

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
            expectWithinAbsoluteError ((double) *fresh.apvts.getRawParameterValue ("intensity"), 0.9, 0.001,
                                       "intensity must survive the round-trip");
            expectWithinAbsoluteError ((double) *fresh.apvts.getRawParameterValue ("complexity"), 0.1, 0.001,
                                       "complexity must survive the round-trip");
        }

        beginTest ("pre-GrooveTable (v1) session migrates its density into intensity/complexity");
        {
            // Build a v1-shaped state: a real saved session, but with the
            // intensity/complexity <PARAM> elements stripped out entirely --
            // exactly what a session saved before those parameters existed
            // looks like (replaceState() then leaves them at their built-in
            // default, which setStateInformation() must detect and correct).
            AdaptiveDrummerProcessor seed;
            auto* densityParam = seed.apvts.getParameter ("density");
            densityParam->setValueNotifyingHost (densityParam->convertTo0to1 (2.0f));   // Full

            juce::MemoryBlock seedState;
            seed.getStateInformation (seedState);

            auto xml = juce::AudioProcessor::getXmlFromBinary (seedState.getData(), (int) seedState.getSize());
            expect (xml != nullptr, "seed state must parse as XML");

            if (auto* e = xml->getChildByAttribute ("id", "intensity"))  xml->removeChildElement (e, true);
            if (auto* e = xml->getChildByAttribute ("id", "complexity")) xml->removeChildElement (e, true);
            expect (xml->getChildByAttribute ("id", "intensity") == nullptr,
                    "premise: the v1-shaped state must genuinely lack an intensity element");

            juce::MemoryBlock v1State;
            juce::AudioProcessor::copyXmlToBinary (*xml, v1State);

            AdaptiveDrummerProcessor fresh;
            fresh.setStateInformation (v1State.getData(), (int) v1State.getSize());

            float expectedComplexity = 0.0f, expectedIntensity = 0.0f;
            DrumPattern::mapLegacy (DrumPattern::Density::Full, expectedComplexity, expectedIntensity);

            expectEquals ((int) *fresh.apvts.getRawParameterValue ("density"), 2,
                          "density itself must still restore correctly");
            expectWithinAbsoluteError ((double) *fresh.apvts.getRawParameterValue ("complexity"),
                                       (double) expectedComplexity, 0.001,
                                       "complexity must be derived from the restored legacy density, "
                                       "not left at the plain default");
            expectWithinAbsoluteError ((double) *fresh.apvts.getRawParameterValue ("intensity"),
                                       (double) expectedIntensity, 0.001,
                                       "intensity must be derived from the restored legacy density, "
                                       "not left at the plain default");
        }
    }
};

static AdaptiveDrummerProcessorTest adaptiveDrummerProcessorTest;
