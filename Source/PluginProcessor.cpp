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
    envelopes.prepare (sampleRate);
    placement.prepare (sampleRate);

    driveStage.prepare (sampleRate);
    collimatorL.prepare (sampleRate);
    collimatorR.prepare (sampleRate);
    stereoSpread.prepare (sampleRate);
    midWobble.prepare (sampleRate);
    voiceL.prepare (sampleRate);
    voiceR.prepare (sampleRate);
    unityMatch.prepare (sampleRate);
    afterglow.prepare (sampleRate);
    limiter.prepare (sampleRate);

    // Worst case is one block of the shortest grid step, each fanning out to
    // the full sub-event count. Reserved once so processBlock never allocates.
    const auto shortestStep = squelch::gridBeats[std::size (squelch::gridBeats) - 1] * 60.0 / 20.0;
    const auto maxSteps = static_cast<int> (samplesPerBlock / (shortestStep * sampleRate)) + 4;
    pendingEvents.reserve (static_cast<size_t> (maxSteps * squelch::dsp::kMaxSubEvents));

    dryDelay.setSize (2, preMixLatency());
    dryDelay.clear();
    dryDelayPos = 0;

    engineAlign.setSize (2, squelch::dsp::kOversamplerLatencySamples);
    engineAlign.clear();
    engineAlignPos = 0;
    timelinePosition = 0;

    refreshReactionSettings();

    // Everything before the mix (the engine's own oversampler plus drive's)
    // plus the limiter, which delays the mixed signal after it. Reported as
    // one figure that does not move with REACTION, or the host re-syncs every
    // time the reaction is switched.
    setLatencySamples (preMixLatency() + squelch::dsp::PeakLimiter::latencySamples (sampleRate));
}

int SquelchAudioProcessor::preMixLatency() const
{
    // SLUDGE's oversampler and the drive stage's, which every reaction passes
    // through. Non-SLUDGE engines are padded to match.
    return 2 * squelch::dsp::kOversamplerLatencySamples;
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
    schedule.mode = static_cast<dsp::Mode> (juce::jlimit (0, 3,
                        static_cast<int> (apvts.getRawParameterValue (ids::mode)->load())));
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

    dsp::EnvelopeParams envelope;
    envelope.spread = spread;
    envelope.decay = decay;
    envelope.exposure = exposure;
    envelope.toxicity = toxicity;
    envelope.containment = value (ids::containment);
    envelope.halfLife = value (ids::halfLife);
    envelope.ionizeAmount = apvts.getRawParameterValue (ids::ionize)->load() > 0.5f
                          ? value (ids::ionizeAmount) : 0.0;
    envelope.seed = seed;
    envelopes.configure ({}, envelope);

    // Each reaction's own appetite for DRIVE and for stereo dispersal, in the
    // order of reactionNames. FISSION scatters across the field because
    // splitting is what it does; the rest stay more centred.
    static constexpr double driveWeights[] { 0.90, 0.70, 1.30, 1.00, 0.80 };
    static constexpr double stereoWeights[] { 0.30, 1.00, 0.20, 0.35, 0.50 };
    const auto index = juce::jlimit (0, 4, currentReaction);

    driveStage.set (value (ids::drive), driveWeights[index]);    collimatorL.set (value (ids::collimator));
    collimatorR.set (value (ids::collimator));
    stereoSpread.set (value (ids::fallout) * stereoWeights[index]);

    afterglowAmount = value (ids::afterglow);
    afterglow.set (afterglowAmount, 0.45);

    // FISSION scatters across the field; the rest scatter in pitch instead,
    // so they stay centred and physical.
    static constexpr double wobbleWeights[] { 1.00, 0.25, 0.90, 0.55, 1.00 };
    midWobble.set (value (ids::fallout) * wobbleWeights[index]);

    static constexpr double persistence[] { 0.60, 0.60, 1.00, 0.85, 0.50 };
    static constexpr double decayLo[] { 0.025, 0.08, 0.25, 0.05, 0.04 };
    static constexpr double decayHi[] { 0.3, 0.6, 1.60, 0.45, 0.5 };
    placement.configure (volatility, envelope.ionizeAmount, decayLo[index],
                         decayHi[index], decay, persistence[index], value (ids::halfLife));
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
    const auto latency = preMixLatency();
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

    const auto fire = [this, reaction] (const squelch::dsp::ScheduledEvent& e)
    {
        if (reaction == "ALIEN")
            alien.trigger (e.index, e.accent, e.pan, 0.0);
        else if (reaction == "CHEMICAL")
            chemical.setEvent (e.index);
        else if (reaction == "RADIATION")
            radiation.trigger (e.accent);

        // Only a slid event uses the distance, and the scheduler places
        // step k from k alone, so the next one is computable rather than
        // needing audio lookahead. A step ahead is close enough: events
        // inside a step are what MAX_SUB_EVENTS fans out.
        const auto step = static_cast<std::int64_t> (scheduler.stepSeconds() * getSampleRate());
        envelopes.trigger (e, std::max<std::int64_t> (step, 1));
        placement.trigger (e);
        midWobble.trigger();
    };

    for (int sample = 0; sample < numSamples; ++sample)
    {
        const auto position = timelinePosition + sample;

        // Each event fires at its own sample, not at the edge of whatever
        // block it landed in. Firing at the block start makes the output
        // depend on the host's buffer size.
        for (const auto& e : pendingEvents)
            if (e.start == position)
                fire (e);

        const auto in = inputGain.getNextValue();
        const auto out = outputGain.getNextValue();
        const auto wet = wetMix.getNextValue();

        const auto dryL = buffer.getSample (0, sample) * in;
        const auto dryR = channels > 1 ? buffer.getSample (1, sample) * in : dryL;

        // INPUT mode takes its events from onsets in the audio rather than
        // from the grid, so it fires inside the sample loop.
        scheduler.detectOnsets (std::max (std::abs (dryL), std::abs (dryR)), position, fire);

        // SLUDGE's oversampler lags by `latency`, so the dry path is delayed
        // by the same amount or the mix comb-filters the two against each
        // other. Every reaction rides the same delay so switching reaction
        // never re-syncs the host.
        const auto delayedL = dryDelay.getSample (0, dryDelayPos);
        const auto delayedR = dryDelay.getSample (1, dryDelayPos);
        dryDelay.setSample (0, dryDelayPos, dryL);
        dryDelay.setSample (1, dryDelayPos, dryR);
        dryDelayPos = (dryDelayPos + 1 == latency) ? 0 : (dryDelayPos + 1);

        const auto env = envelopes.process();

        double wetL = 0.0, wetR = 0.0;

        if (reaction == "SLUDGE")
            sludge.process (dryL, dryR, wetL, wetR);
        else if (reaction == "ALIEN")
            alien.process (wetL, wetR);
        else if (reaction == "CHEMICAL")
        {
            wetL = chemical.process (dryL, env.cutoffHz, env.feedback, env.drive);
            wetR = chemical.process (dryR, env.cutoffHz, env.feedback, env.drive);
        }
        else if (reaction == "RADIATION")
            radiation.process (dryL, dryR, wetL, wetR);
        else if (reaction == "FISSION")
            fission.process (dryL, dryR, wetL, wetR);

        // Only SLUDGE lags on its own, so the rest are padded to match and the
        // reported latency stays put when REACTION changes.
        if (reaction != "SLUDGE")
        {
            const auto heldL = engineAlign.getSample (0, engineAlignPos);
            const auto heldR = engineAlign.getSample (1, engineAlignPos);
            engineAlign.setSample (0, engineAlignPos, static_cast<float> (wetL));
            engineAlign.setSample (1, engineAlignPos, static_cast<float> (wetR));
            engineAlignPos = (engineAlignPos + 1 == engineAlign.getNumSamples()) ? 0 : (engineAlignPos + 1);
            wetL = heldL;
            wetR = heldR;
        }

        // The output chain. Placement and AFTERGLOW are absent: pan_gain comes
        // from build_controls and the glow needs a reverb, neither of which is
        // ported, so they are left out rather than approximated.
        driveStage.process (wetL, wetR, wetL, wetR);
        wetL = collimatorL.process (wetL);
        wetR = collimatorR.process (wetR);

        // Placement runs AFTER the saturation, not before it. A hard-panned
        // event has one loud channel and one quiet one, and tanh compresses
        // the loud one harder, so driving a placed signal squeezes most of
        // the placement back out: 7.5 dB of balance swing down to 3.6.
        const auto place = placement.process();
        wetL *= place.panL;
        wetR *= place.panR;

        if (afterglowAmount > 0.0)
        {
            // Each event's own send decides how far back it sits, so some
            // arrive close and dry while others wash back.
            double glowL = 0.0, glowR = 0.0;
            afterglow.process (wetL * place.send, wetR * place.send, glowL, glowR);
            wetL += glowL * (0.9 * afterglowAmount);
            wetR += glowR * (0.9 * afterglowAmount);
        }

        stereoSpread.process (wetL, wetR, wetL, wetR);
        midWobble.process (wetL, wetR, wetL, wetR);
        wetL = voiceL.process (wetL);
        wetR = voiceR.process (wetR);
        unityMatch.process (wetL, wetR, wetL, wetR);

        const auto mixedL = delayedL + (static_cast<float> (wetL) - delayedL) * wet;
        const auto mixedR = delayedR + (static_cast<float> (wetR) - delayedR) * wet;

        // Peak safety runs after the blend, not on the wet path alone: the
        // reaction decorrelates phase against the dry, so the mix can peak
        // higher than either part did on its own.
        double safeL = 0.0, safeR = 0.0;
        limiter.process (mixedL, mixedR, safeL, safeR);

        buffer.setSample (0, sample, static_cast<float> (safeL) * out);
        if (channels > 1)
            buffer.setSample (1, sample, static_cast<float> (safeR) * out);
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
