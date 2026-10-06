#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Parameters.h"
#include "Presets.h"
#include "Dsp/Alien.h"
#include "Dsp/Chemical.h"
#include "Dsp/Envelopes.h"
#include "Dsp/OutputStage.h"
#include "Dsp/Placement.h"
#include "Dsp/Fission.h"
#include "Dsp/MidWobble.h"
#include "Dsp/Meltdown.h"
#include "Dsp/NoiseBed.h"
#include "Dsp/Radiation.h"
#include "Dsp/Reverb.h"
#include "Dsp/Scheduler.h"
#include "Dsp/Sludge.h"

class SquelchAudioProcessor : public juce::AudioProcessor
{
public:
    SquelchAudioProcessor();
    ~SquelchAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;

    /// Input trim and output level are session gain staging, so they are held
    /// here rather than in the APVTS: never in a preset, never randomised.
    std::atomic<float> inputTrimDb { 0.0f };
    std::atomic<float> outputTrimDb { 0.0f };
    std::atomic<float> mix { 1.0f };

    /// Which preset the toolbar has selected, so a closed and reopened editor shows the same
    /// one. Saved with the session; a session saved before presets existed reads as Default.
    std::atomic<int> presetIndex { squelch::presets::notChosen };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    /// Reads the host's transport at the start of a block and sets `timelinePosition`.
    /// `wrapAt` is the sample at which a host loop ends inside this block, or -1, and
    /// `wrapTo` where the timeline then continues from.
    void followTransport (int numSamples, int& wrapAt, std::int64_t& wrapTo);

    /// Reads the APVTS once per block, off the audio thread's hot loop.
    void refreshReactionSettings();

    /// Latency ahead of the dry/wet mix, which the dry path has to match.
    int preMixLatency() const;

    juce::SmoothedValue<float> inputGain, outputGain, wetMix;

    /// ENRICHMENT as a linear gain into the engines. The dry path does not get it.
    juce::SmoothedValue<float> enrichmentGain;
    float currentEnrichmentGain() const;

    /// Staged values for the eight parameters MELTDOWN pushes. Sampled once per
    /// block and advanced by the block's length at the end of it.
    squelch::dsp::Meltdown meltdown;

    squelch::dsp::SludgeEngine sludge;
    squelch::dsp::AlienEngine alien;
    squelch::dsp::ChemicalEngine chemical;
    squelch::dsp::RadiationEngine radiation;
    squelch::dsp::FissionEngine fission;

    squelch::dsp::Scheduler scheduler;
    squelch::dsp::Envelopes envelopes;
    squelch::dsp::Placement placement;
    squelch::dsp::NoiseBed noiseBed;

    squelch::dsp::Drive driveStage;
    squelch::dsp::Collimator collimatorL, collimatorR;
    squelch::dsp::StereoSpread stereoSpread;
    squelch::dsp::MidWobble midWobble;
    squelch::dsp::Voice voiceL, voiceR;
    squelch::dsp::UnityMatch unityMatch;
    squelch::dsp::Reverb afterglow;
    double afterglowAmount { 0.0 };
    squelch::dsp::PeakLimiter limiter;
    squelch::dsp::HardClip hardClip;
    bool clipOn { false };

    /// Only SLUDGE carries its own oversampler lag. The others are delayed by
    /// the same amount so the reported latency does not move with REACTION.
    juce::AudioBuffer<float> engineAlign;
    int engineAlignPos { 0 };

    /// Events for the current block, collected once and then fired at their
    /// own sample positions. Reserved in prepareToPlay: push_back on the
    /// audio thread must never allocate, and events past the reservation are
    /// dropped rather than grow it.
    std::vector<squelch::dsp::ScheduledEvent> pendingEvents, pendingWrapEvents;

    /// Where the host is on its timeline. Taken from the playhead rather than
    /// counted locally, or events detach from the timeline on a locate and
    /// the same bar stops bouncing identically.
    std::int64_t timelinePosition { 0 };

    /// Host tempo, so the grid follows the session. 120 is JUCE's own fallback
    /// for a host that reports none.
    double hostBpm { 120.0 };
    double preparedSampleRate { 44100.0 };

    /// SLUDGE runs its saturation through a causal oversampler, which lags by
    /// kOversamplerLatencySamples. The dry path is delayed to match so the mix
    /// does not comb-filter, and the same figure is reported to the host.
    juce::AudioBuffer<float> dryDelay;
    int dryDelayPos { 0 };

    int currentReaction { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SquelchAudioProcessor)
};
