#include "PluginProcessor.h"

#include "PluginEditor.h"

namespace
{
    constexpr float smoothingSeconds = 0.02f;
}

SquelchAudioProcessor::SquelchAudioProcessor()
    : juce::AudioProcessor (BusesProperties()
                                .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "SQUELCH", createParameterLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout
SquelchAudioProcessor::createParameterLayout()
{
    using namespace squelch;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    const auto version = juce::ParameterID { "", 1 };

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { ids::reaction, 1 }, "Reaction", reactionNames, 0));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { ids::mode, 1 }, "Mode", modeNames, 0));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { ids::grid, 1 }, "Grid", gridNames, 8));

    for (const auto& p : continuous)
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { p.id, 1 }, p.label,
            juce::NormalisableRange<float> { 0.0f, 1.0f }, p.defaultValue));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { ids::ionize, 1 }, "Ionize", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { ids::meltdown, 1 }, "Meltdown", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { ids::clip, 1 }, "Clip", false));

    // Hashed with the step index for every probabilistic choice, so a given
    // seed always produces the same pattern.
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { ids::seed, 1 }, "Seed", 0, 999, 0));

    juce::ignoreUnused (version);
    return layout;
}

void SquelchAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);

    for (auto* value : { &inputGain, &outputGain, &wetMix })
        value->reset (sampleRate, smoothingSeconds);

    inputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (inputTrimDb.load()));
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputTrimDb.load()));
    wetMix.setCurrentAndTargetValue (mix.load());

    // Both ceilings delay by the limiter's lookahead, so this figure does not
    // change when CLIP is switched and the host never has to re-sync.
    setLatencySamples (static_cast<int> (0.005 * sampleRate));
}

double SquelchAudioProcessor::getTailLengthSeconds() const
{
    // Afterglow's longest measured RT60 is just over three seconds.
    return 3.5;
}

bool SquelchAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void SquelchAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                          juce::MidiBuffer& midi)
{
    juce::ignoreUnused (midi);
    juce::ScopedNoDenormals noDenormals;

    for (auto i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    inputGain.setTargetValue (juce::Decibels::decibelsToGain (inputTrimDb.load()));
    outputGain.setTargetValue (juce::Decibels::decibelsToGain (outputTrimDb.load()));
    wetMix.setTargetValue (mix.load());

    // The reaction itself is not ported yet. Everything above is in place so
    // the plugin loads, validates and gain stages; the DSP lands next.
    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        const auto in = inputGain.getNextValue();
        const auto out = outputGain.getNextValue();
        wetMix.getNextValue();

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            buffer.setSample (channel, sample, buffer.getSample (channel, sample) * in * out);
    }
}

juce::AudioProcessorEditor* SquelchAudioProcessor::createEditor()
{
    return new SquelchAudioProcessorEditor (*this);
}

void SquelchAudioProcessor::getStateInformation (juce::MemoryBlock& destination)
{
    auto state = apvts.copyState();

    // The strips are session gain staging, so they ride with the DAW project
    // rather than with a preset.
    state.setProperty ("inputTrimDb", inputTrimDb.load(), nullptr);
    state.setProperty ("outputTrimDb", outputTrimDb.load(), nullptr);
    state.setProperty ("mix", mix.load(), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destination);
}

void SquelchAudioProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    apvts.replaceState (state);

    inputTrimDb = static_cast<float> (state.getProperty ("inputTrimDb", 0.0));
    outputTrimDb = static_cast<float> (state.getProperty ("outputTrimDb", 0.0));
    mix = static_cast<float> (state.getProperty ("mix", 1.0));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SquelchAudioProcessor();
}
