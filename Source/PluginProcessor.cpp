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
    chemical.prepare (sampleRate);
    radiation.prepare (sampleRate);
    fission.prepare (sampleRate);
    scheduler.prepare (sampleRate);

    // Worst case is one block of the shortest grid step, each fanning out to
    // the full sub-event count. Reserved once so processBlock never allocates.
    const auto shortestStep = squelch::gridBeats[std::size (squelch::gridBeats) - 1] * 60.0 / 20.0;
    const auto maxSteps = static_cast<int> (samplesPerBlock / (shortestStep * sampleRate)) + 4;
    pendingEvents.reserve (static_cast<size_t> (maxSteps * squelch::dsp::kMaxSubEvents));

    dryDelay.setSize (2, squelch::dsp::kOversamplerLatencySamples);
    dryDelay.clear();
    dryDelayPos = 0;
    timelinePosition = 0;

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

    const auto spread = value (ids::spread);
    const auto decay = value (ids::decay);
    const auto exposure = value (ids::exposure);
    const auto toxicity = value (ids::toxicity);
    const auto reactivity = value (ids::reactivity);
    const auto volatility = value (ids::volatility);
    const auto seed = static_cast<std::uint64_t> (value (ids::seed));

    dsp::ScheduleSettings schedule;
    schedule.gridIndex = static_cast<int> (apvts.getRawParameterValue (ids::grid)->load());
    schedule.bpm = hostBpm;
    schedule.flux = value (ids::flux);
    schedule.probability = value (ids::probability);
    schedule.reactivity = reactivity;
    schedule.volatility = volatility;
    schedule.containment = value (ids::containment);
    schedule.seed = seed;
    scheduler.configure (schedule);

    sludge.configure ({}, { decay, value (ids::halfLife), spread, reactivity, exposure, toxicity });
    alien.configure ({}, { spread, decay, toxicity, exposure, seed });
    radiation.configure ({}, { volatility, spread, decay, exposure, seed });
    fission.configure ({}, { spread, decay, exposure, seed });
    chemical.setSeed (seed);
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

    if (auto* playHead = getPlayHead())
        if (const auto position = playHead->getPosition())
        {
            if (const auto samples = position->getTimeInSamples())
                timelinePosition = *samples;
            if (const auto bpm = position->getBpm())
                hostBpm = *bpm;
        }

    refreshReactionSettings();

    const auto channels = buffer.getNumChannels();
    const auto latency = squelch::dsp::kOversamplerLatencySamples;
    const auto numSamples = buffer.getNumSamples();

    pendingEvents.clear();
    scheduler.forRange (timelinePosition, numSamples,
                        [this] (const squelch::dsp::ScheduledEvent& e)
                        {
                            // Dropped rather than grown: allocating here would
                            // be worse than losing an event at absurd density.
                            if (pendingEvents.size() < pendingEvents.capacity())
                                pendingEvents.push_back (e);
                        });

    const auto reaction = squelch::reactionNames[currentReaction];

    for (int sample = 0; sample < numSamples; ++sample)
    {
        const auto position = timelinePosition + sample;

        // Each event fires at its own sample, not at the edge of whatever
        // block it landed in. Firing at the block start makes the output
        // depend on the host's buffer size.
        for (const auto& e : pendingEvents)
        {
            if (e.start != position)
                continue;

            if (reaction == "ALIEN")
                alien.trigger (e.index, e.accent, e.pan, 0.0);
            else if (reaction == "CHEMICAL")
                chemical.setEvent (e.index);
            else if (reaction == "RADIATION")
                radiation.trigger (e.accent);
        }

        const auto in = inputGain.getNextValue();
        const auto out = outputGain.getNextValue();
        const auto wet = wetMix.getNextValue();

        const auto dryL = buffer.getSample (0, sample) * in;
        const auto dryR = channels > 1 ? buffer.getSample (1, sample) * in : dryL;

        // SLUDGE's oversampler lags by `latency`, so the dry path is delayed
        // by the same amount or the mix comb-filters the two against each
        // other. Every reaction rides the same delay so switching reaction
        // never re-syncs the host.
        const auto delayedL = dryDelay.getSample (0, dryDelayPos);
        const auto delayedR = dryDelay.getSample (1, dryDelayPos);
        dryDelay.setSample (0, dryDelayPos, dryL);
        dryDelay.setSample (1, dryDelayPos, dryR);
        dryDelayPos = (dryDelayPos + 1 == latency) ? 0 : (dryDelayPos + 1);

        double wetL = delayedL, wetR = delayedR;

        if (reaction == "SLUDGE")
            sludge.process (dryL, dryR, wetL, wetR);
        else if (reaction == "ALIEN")
            alien.process (wetL, wetR);
        else if (reaction == "CHEMICAL")
        {
            wetL = chemical.process (dryL, 700.0, 2.2, 1.4);
            wetR = chemical.process (dryR, 700.0, 2.2, 1.4);
        }
        else if (reaction == "RADIATION")
            radiation.process (dryL, dryR, wetL, wetR);
        else if (reaction == "FISSION")
            fission.process (dryL, dryR, wetL, wetR);

        const auto mixedL = delayedL + (static_cast<float> (wetL) - delayedL) * wet;
        const auto mixedR = delayedR + (static_cast<float> (wetR) - delayedR) * wet;

        buffer.setSample (0, sample, mixedL * out);
        if (channels > 1)
            buffer.setSample (1, sample, mixedR * out);
    }

    timelinePosition += numSamples;
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
