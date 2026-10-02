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

    sludge.prepare (sampleRate);
    alien.prepare (sampleRate);

    dryDelay.setSize (2, squelch::dsp::kOversamplerLatencySamples);
    dryDelay.clear();
    dryDelayPos = 0;

    refreshReactionSettings();

    // SLUDGE's causal oversampler is the only latency the plugin currently
    // has. The previous figure here was the output stage's limiter lookahead,
    // which is not ported yet and so was latency the host was told about but
    // never actually given.
    setLatencySamples (squelch::dsp::kOversamplerLatencySamples);
}

void SquelchAudioProcessor::refreshReactionSettings()
{
    using namespace squelch;

    const auto value = [this] (const char* id)
    {
        return static_cast<double> (apvts.getRawParameterValue (id)->load());
    };

    currentReaction = static_cast<int> (apvts.getRawParameterValue (ids::reaction)->load());

    dsp::SludgeProfile profile;
    dsp::SludgeParams params;
    params.decay = value (ids::decay);
    params.halfLife = value (ids::halfLife);
    params.spread = value (ids::spread);
    params.reactivity = value (ids::reactivity);
    params.exposure = value (ids::exposure);
    params.toxicity = value (ids::toxicity);
    sludge.configure (profile, params);
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

    refreshReactionSettings();

    // Only SLUDGE is wired. ALIEN is ported and verified but is event-gated,
    // and the scheduler it needs is not ported yet, so it has nothing to
    // trigger it; the others have no engine here at all. Those reactions pass
    // the dry signal through rather than pretending to react.
    const auto sludgeSelected = currentReaction == squelch::reactionNames.indexOf ("SLUDGE");

    const auto channels = buffer.getNumChannels();
    const auto latency = squelch::dsp::kOversamplerLatencySamples;

    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
    {
        const auto in = inputGain.getNextValue();
        const auto out = outputGain.getNextValue();
        const auto wet = wetMix.getNextValue();

        const auto dryL = buffer.getSample (0, sample) * in;
        const auto dryR = channels > 1 ? buffer.getSample (1, sample) * in : dryL;

        // SLUDGE's oversampler lags by `latency`, so the dry path is delayed
        // by the same amount or the mix comb-filters the two against each
        // other. Reactions without an engine ride the same delay so switching
        // reaction never re-syncs the host.
        const auto delayedL = dryDelay.getSample (0, dryDelayPos);
        const auto delayedR = dryDelay.getSample (1, dryDelayPos);
        dryDelay.setSample (0, dryDelayPos, dryL);
        dryDelay.setSample (1, dryDelayPos, dryR);
        dryDelayPos = (dryDelayPos + 1 == latency) ? 0 : (dryDelayPos + 1);

        auto wetL = delayedL;
        auto wetR = delayedR;

        if (sludgeSelected)
        {
            double sl = 0.0, sr = 0.0;
            sludge.process (dryL, dryR, sl, sr);
            wetL = static_cast<float> (sl);
            wetR = static_cast<float> (sr);
        }

        const auto mixedL = delayedL + (wetL - delayedL) * wet;
        const auto mixedR = delayedR + (wetR - delayedR) * wet;

        buffer.setSample (0, sample, mixedL * out);
        if (channels > 1)
            buffer.setSample (1, sample, mixedR * out);
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
