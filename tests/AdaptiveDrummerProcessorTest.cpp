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

        // The processor now declares two input buses (main + Sidechain), so a
        // structurally complete layout needs an entry for both — checkBusesLayoutSupported()
        // itself rejects a layout with the wrong number of buses before our
        // isBusesLayoutSupported() override ever runs. Existing main-bus-only
        // tests get a disabled Sidechain (its default), unaffected by their intent.
        auto makeLayout = [] (juce::AudioChannelSet in, juce::AudioChannelSet out)
        {
            juce::AudioProcessor::BusesLayout layout;
            layout.inputBuses.add (in);
            layout.outputBuses.add (out);
            layout.inputBuses.add (juce::AudioChannelSet::disabled());   // Sidechain, off
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

        beginTest ("bus layouts — sidechain configurations");
        {
            AdaptiveDrummerProcessor proc;

            auto makeSidechainLayout = [] (juce::AudioChannelSet sidechain)
            {
                juce::AudioProcessor::BusesLayout layout;
                layout.inputBuses.add (juce::AudioChannelSet::stereo());
                layout.outputBuses.add (juce::AudioChannelSet::stereo());
                layout.inputBuses.add (sidechain);
                return layout;
            };

            expect (proc.checkBusesLayoutSupported (makeSidechainLayout (juce::AudioChannelSet::disabled())),
                    "disabled sidechain must be accepted (the default)");
            expect (proc.checkBusesLayoutSupported (makeSidechainLayout (juce::AudioChannelSet::mono())),
                    "mono sidechain must be accepted");
            expect (proc.checkBusesLayoutSupported (makeSidechainLayout (juce::AudioChannelSet::stereo())),
                    "stereo sidechain must be accepted");
            expect (! proc.checkBusesLayoutSupported (makeSidechainLayout (juce::AudioChannelSet::quadraphonic())),
                    "a quadraphonic sidechain must be rejected");
        }

        beginTest ("Follow prioritises a connected Sidechain bus over the main input");
        {
            // Proves the guide is genuinely read from the Sidechain bus (a
            // user-chosen *other* track), not the effect's own input: the main
            // input stays silent throughout, only the Sidechain carries a loud
            // tone, and Follow's energy must still rise.
            AdaptiveDrummerProcessor proc;

            const double sampleRate = 44100.0;
            const int    blockSize  = 512;

            juce::AudioProcessor::BusesLayout layout;
            layout.inputBuses.add  (juce::AudioChannelSet::stereo());   // main in (left silent)
            layout.outputBuses.add (juce::AudioChannelSet::stereo());   // main out
            layout.inputBuses.add  (juce::AudioChannelSet::stereo());   // sidechain, enabled
            expect (proc.setBusesLayout (layout), "the 3-bus (sidechain-enabled) layout must be settable");

            proc.prepareToPlay (sampleRate, blockSize);
            proc.apvts.getParameter ("follow")->setValueNotifyingHost (1.0f);

            const int totalChannels = juce::jmax (proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
            expectEquals (totalChannels, 4, "main (2ch) + sidechain (2ch) must total 4 input channels");

            juce::AudioBuffer<float> buffer (totalChannels, blockSize);
            juce::MidiBuffer midi;

            double phase = 0.0;
            const double phaseInc = juce::MathConstants<double>::twoPi * 440.0 / sampleRate;

            for (int b = 0; b < 40; ++b)
            {
                buffer.clear();   // channels 0-1 (main in) stay silent
                for (int ch = 2; ch < 4; ++ch)   // channels 2-3 = Sidechain bus
                {
                    float* d = buffer.getWritePointer (ch);
                    double  p = phase;
                    for (int i = 0; i < blockSize; ++i)
                    {
                        d[i] = 0.9f * (float) std::sin (p);
                        p += phaseInc;
                    }
                }
                phase += phaseInc * blockSize;

                proc.processBlock (buffer, midi);
            }

            expectGreaterThan (proc.getEnergy(), 0.3f,
                                "a loud Sidechain guide must raise the followed energy even "
                                "though the main input is silent");

            proc.releaseResources();
        }

        beginTest ("Follow never changes the complexity readout, even if the parameter moves mid-run");
        {
            AdaptiveDrummerProcessor proc;

            const double sampleRate = 44100.0;
            const int    blockSize  = 512;

            juce::AudioProcessor::BusesLayout layout;
            layout.inputBuses.add  (juce::AudioChannelSet::stereo());
            layout.outputBuses.add (juce::AudioChannelSet::stereo());
            layout.inputBuses.add  (juce::AudioChannelSet::stereo());   // sidechain, enabled
            expect (proc.setBusesLayout (layout), "the 3-bus (sidechain-enabled) layout must be settable");

            proc.prepareToPlay (sampleRate, blockSize);
            proc.apvts.getParameter ("follow")->setValueNotifyingHost (1.0f);

            auto* complexityParam = proc.apvts.getParameter ("complexity");
            const int totalChannels = juce::jmax (proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
            juce::AudioBuffer<float> buffer (totalChannels, blockSize);
            juce::MidiBuffer midi;

            double phase = 0.0;
            const double phaseInc = juce::MathConstants<double>::twoPi * 440.0 / sampleRate;
            auto renderLoudSidechainBlock = [&]
            {
                buffer.clear();
                for (int ch = 2; ch < 4; ++ch)
                {
                    float* d = buffer.getWritePointer (ch);
                    double p = phase;
                    for (int i = 0; i < blockSize; ++i) { d[i] = 0.9f * (float) std::sin (p); p += phaseInc; }
                }
                phase += phaseInc * blockSize;
                proc.processBlock (buffer, midi);
            };

            complexityParam->setValueNotifyingHost (complexityParam->convertTo0to1 (0.3f));
            const float expected1 = *proc.apvts.getRawParameterValue ("complexity");
            for (int b = 0; b < 40; ++b)
            {
                renderLoudSidechainBlock();
                expectEquals (proc.getCurrentComplexity(), expected1,
                              "complexity must equal the grid parameter on every single block");
            }

            complexityParam->setValueNotifyingHost (complexityParam->convertTo0to1 (0.8f));   // change mid-run
            const float expected2 = *proc.apvts.getRawParameterValue ("complexity");
            for (int b = 0; b < 40; ++b)
            {
                renderLoudSidechainBlock();
                expectEquals (proc.getCurrentComplexity(), expected2,
                              "complexity must track a parameter change instantly, still exactly the grid's value");
            }

            proc.releaseResources();
        }

        beginTest ("Follow modulates intensity around the grid's base — up when loud, down when silent");
        {
            AdaptiveDrummerProcessor proc;

            const double sampleRate = 44100.0;
            const int    blockSize  = 512;

            juce::AudioProcessor::BusesLayout layout;
            layout.inputBuses.add  (juce::AudioChannelSet::stereo());   // main in (silent throughout)
            layout.outputBuses.add (juce::AudioChannelSet::stereo());
            layout.inputBuses.add  (juce::AudioChannelSet::stereo());   // sidechain, enabled
            expect (proc.setBusesLayout (layout), "the 3-bus (sidechain-enabled) layout must be settable");

            proc.prepareToPlay (sampleRate, blockSize);
            proc.apvts.getParameter ("follow")->setValueNotifyingHost (1.0f);

            auto* intensityParam = proc.apvts.getParameter ("intensity");
            intensityParam->setValueNotifyingHost (intensityParam->convertTo0to1 (0.5f));
            const float base = *proc.apvts.getRawParameterValue ("intensity");

            const int totalChannels = juce::jmax (proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
            juce::AudioBuffer<float> buffer (totalChannels, blockSize);
            juce::MidiBuffer midi;

            double phase = 0.0;
            const double phaseInc = juce::MathConstants<double>::twoPi * 440.0 / sampleRate;
            auto feedSidechain = [&] (float amplitude, int numBlocks)
            {
                for (int b = 0; b < numBlocks; ++b)
                {
                    buffer.clear();
                    for (int ch = 2; ch < 4; ++ch)
                    {
                        float* d = buffer.getWritePointer (ch);
                        double p = phase;
                        for (int i = 0; i < blockSize; ++i) { d[i] = amplitude * (float) std::sin (p); p += phaseInc; }
                    }
                    phase += phaseInc * blockSize;
                    proc.processBlock (buffer, midi);
                }
            };

            feedSidechain (0.9f, 100);   // loud guide, long enough for the macro envelope to settle up
            expectGreaterThan (proc.getCurrentIntensity(), base,
                                "a loud connected guide must raise the effective intensity above the grid's base");

            feedSidechain (0.0f, 100);   // sidechain still enabled/connected, but silent now
            expectLessThan (proc.getCurrentIntensity(), base,
                            "a connected-but-silent guide must lower the effective intensity below the grid's base");

            proc.releaseResources();
        }

        beginTest ("followAmount == 0 fully bypasses Follow's modulation");
        {
            AdaptiveDrummerProcessor proc;

            const double sampleRate = 44100.0;
            const int    blockSize  = 512;

            juce::AudioProcessor::BusesLayout layout;
            layout.inputBuses.add  (juce::AudioChannelSet::stereo());
            layout.outputBuses.add (juce::AudioChannelSet::stereo());
            layout.inputBuses.add  (juce::AudioChannelSet::stereo());   // sidechain, enabled
            expect (proc.setBusesLayout (layout), "the 3-bus (sidechain-enabled) layout must be settable");

            proc.prepareToPlay (sampleRate, blockSize);
            proc.apvts.getParameter ("follow")->setValueNotifyingHost (1.0f);
            proc.apvts.getParameter ("followAmount")->setValueNotifyingHost (0.0f);   // fully off

            auto* intensityParam = proc.apvts.getParameter ("intensity");
            intensityParam->setValueNotifyingHost (intensityParam->convertTo0to1 (0.5f));
            const float base = *proc.apvts.getRawParameterValue ("intensity");

            const int totalChannels = juce::jmax (proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
            juce::AudioBuffer<float> buffer (totalChannels, blockSize);
            juce::MidiBuffer midi;

            double phase = 0.0;
            const double phaseInc = juce::MathConstants<double>::twoPi * 440.0 / sampleRate;
            for (int b = 0; b < 100; ++b)   // loud guide, long enough that a nonzero amount would clearly show
            {
                buffer.clear();
                for (int ch = 2; ch < 4; ++ch)
                {
                    float* d = buffer.getWritePointer (ch);
                    double p = phase;
                    for (int i = 0; i < blockSize; ++i) { d[i] = 0.9f * (float) std::sin (p); p += phaseInc; }
                }
                phase += phaseInc * blockSize;
                proc.processBlock (buffer, midi);
            }

            expectEquals (proc.getCurrentIntensity(), base,
                          "followAmount == 0 must leave intensity exactly at the grid's base, even with a loud guide");

            proc.releaseResources();
        }

        beginTest ("followAmount survives state round-trip and defaults to 0.6 when absent from a saved session");
        {
            AdaptiveDrummerProcessor proc;
            auto* amountParam = proc.apvts.getParameter ("followAmount");
            amountParam->setValueNotifyingHost (amountParam->convertTo0to1 (0.3f));

            juce::MemoryBlock savedState;
            proc.getStateInformation (savedState);

            AdaptiveDrummerProcessor fresh;
            fresh.setStateInformation (savedState.getData(), (int) savedState.getSize());
            expectWithinAbsoluteError ((double) *fresh.apvts.getRawParameterValue ("followAmount"), 0.3, 0.001,
                                       "followAmount must survive the round-trip");

            // A state saved before this parameter existed (no <PARAM id="followAmount">)
            // must load with the built-in default on a fresh instance, not some
            // garbage/zeroed value -- exactly the same APVTS behaviour the
            // pre-GrooveTable migration test above already relies on.
            auto xml = juce::AudioProcessor::getXmlFromBinary (savedState.getData(), (int) savedState.getSize());
            expect (xml != nullptr, "seed state must parse as XML");
            if (auto* e = xml->getChildByAttribute ("id", "followAmount")) xml->removeChildElement (e, true);
            expect (xml->getChildByAttribute ("id", "followAmount") == nullptr,
                    "premise: the pre-Step-21 state must genuinely lack a followAmount element");

            juce::MemoryBlock preStep21State;
            juce::AudioProcessor::copyXmlToBinary (*xml, preStep21State);

            AdaptiveDrummerProcessor freshOld;
            freshOld.setStateInformation (preStep21State.getData(), (int) preStep21State.getSize());
            expectWithinAbsoluteError ((double) *freshOld.apvts.getRawParameterValue ("followAmount"), 0.6, 0.001,
                                       "a session saved before followAmount existed must load with the 0.6 default");
        }

        beginTest ("Follow off, or on with no guide connected, is an exact manual passthrough");
        {
            // Follow off: the classic case.
            {
                AdaptiveDrummerProcessor proc;
                proc.setPlayConfigDetails (2, 2, 44100.0, 512);
                proc.prepareToPlay (44100.0, 512);

                auto* intensityParam  = proc.apvts.getParameter ("intensity");
                auto* complexityParam = proc.apvts.getParameter ("complexity");
                intensityParam->setValueNotifyingHost  (intensityParam->convertTo0to1  (0.7f));
                complexityParam->setValueNotifyingHost (complexityParam->convertTo0to1 (0.15f));
                const float expectedIntensity  = *proc.apvts.getRawParameterValue ("intensity");
                const float expectedComplexity = *proc.apvts.getRawParameterValue ("complexity");

                juce::AudioBuffer<float> buffer (2, 512);
                juce::MidiBuffer midi;
                buffer.clear();
                proc.processBlock (buffer, midi);

                expectEquals (proc.getCurrentIntensity(),  expectedIntensity,  "Follow off: intensity must equal the grid exactly");
                expectEquals (proc.getCurrentComplexity(), expectedComplexity, "Follow off: complexity must equal the grid exactly");
                proc.releaseResources();
            }

            // Follow on, but no guide at all: disabled main input, no sidechain
            // connected — the guideAvailable == false branch (manual passthrough,
            // a deliberate improvement over following silence down to nothing).
            {
                AdaptiveDrummerProcessor proc;

                juce::AudioProcessor::BusesLayout layout;
                layout.inputBuses.add  (juce::AudioChannelSet::disabled());
                layout.outputBuses.add (juce::AudioChannelSet::stereo());
                layout.inputBuses.add  (juce::AudioChannelSet::disabled());   // sidechain, off
                expect (proc.setBusesLayout (layout), "disabled-main-in layout must be settable");

                proc.prepareToPlay (44100.0, 512);
                proc.apvts.getParameter ("follow")->setValueNotifyingHost (1.0f);

                auto* intensityParam  = proc.apvts.getParameter ("intensity");
                auto* complexityParam = proc.apvts.getParameter ("complexity");
                intensityParam->setValueNotifyingHost  (intensityParam->convertTo0to1  (0.7f));
                complexityParam->setValueNotifyingHost (complexityParam->convertTo0to1 (0.15f));
                const float expectedIntensity  = *proc.apvts.getRawParameterValue ("intensity");
                const float expectedComplexity = *proc.apvts.getRawParameterValue ("complexity");

                const int totalChannels = juce::jmax (proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels());
                juce::AudioBuffer<float> buffer (juce::jmax (2, totalChannels), 512);
                juce::MidiBuffer midi;

                for (int b = 0; b < 10; ++b)
                {
                    buffer.clear();
                    proc.processBlock (buffer, midi);
                }

                expectEquals (proc.getCurrentIntensity(),  expectedIntensity,
                              "Follow on, no guide connected: intensity must still equal the grid exactly");
                expectEquals (proc.getCurrentComplexity(), expectedComplexity,
                              "Follow on, no guide connected: complexity must still equal the grid exactly");
                proc.releaseResources();
            }
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

        beginTest ("getTailLengthSeconds reports a nonzero tail (Step 8)");
        {
            AdaptiveDrummerProcessor proc;
            expectGreaterThan (proc.getTailLengthSeconds(), 0.0,
                               "a zero tail tells hosts to cut audio immediately on stop, "
                               "which is exactly the click this step fixes");
        }

        beginTest ("stopping the transport lets a ringing tail decay instead of cutting off (Step 8)");
        {
            AdaptiveDrummerProcessor proc;

            const double sampleRate = 44100.0;
            const int    blockSize  = 512;

            proc.setPlayConfigDetails (2, 2, sampleRate, blockSize);
            proc.prepareToPlay (sampleRate, blockSize);

            // Full complexity on Rock guarantees a crash on step 0 (~1.2 s decay
            // in DrumSynth), so there's something to still be ringing once the
            // transport stops one block later.
            proc.apvts.getParameter ("complexity")->setValueNotifyingHost (1.0f);
            proc.apvts.getParameter ("play")->setValueNotifyingHost (1.0f);

            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;

            auto anySampleAudible = [&]
            {
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                {
                    const float* d = buffer.getReadPointer (ch);
                    for (int i = 0; i < blockSize; ++i)
                        if (std::abs (d[i]) > 1.0e-3f)
                            return true;
                }
                return false;
            };

            buffer.clear();
            proc.processBlock (buffer, midi);
            expect (anySampleAudible(), "step 0 hit must be audible before stopping");

            // Stop the transport mid-decay.
            proc.apvts.getParameter ("play")->setValueNotifyingHost (0.0f);

            bool everAudibleAfterStop = false;
            for (int b = 0; b < 20 && ! everAudibleAfterStop; ++b)
            {
                buffer.clear();
                proc.processBlock (buffer, midi);
                everAudibleAfterStop = anySampleAudible();
            }

            expect (everAudibleAfterStop,
                    "a ringing tail must keep decaying for a few blocks after the transport "
                    "stops, not go silent on the very next block");

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
