#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Parameters.h"

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

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::SmoothedValue<float> inputGain, outputGain, wetMix;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SquelchAudioProcessor)
};
