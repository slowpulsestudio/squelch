#include "SquelchDisplay.h"

#include <cmath>
#include <sps/Tokens.h>

#include "Parameters.h"

namespace squelch
{
namespace
{
    namespace color = sps::tokens::color;
    namespace vis = sps::visualizer;

    constexpr float minHz = 40.0f;
    constexpr float maxHz = 20000.0f;

    // The time axis spans this many seconds, newest at the front face.
    constexpr float window = 4.0f;

    constexpr int curvePoints = 40;

    juce::String hzText (float hz)
    {
        if (hz >= 10000.0f)
            return juce::String (juce::roundToInt (hz / 1000.0f)) + "k";

        if (hz >= 1000.0f)
            return juce::String (hz / 1000.0f, 1) + "k";

        return juce::String (juce::roundToInt (hz));
    }
}

SquelchDisplay::SquelchDisplay (std::function<DisplayState()> sourceIn) : source (std::move (sourceIn))
{
    setTitle ("SQUELCH");
    setBox ({ 1.0f, 0.62f, 0.55f });

    setAxes ({ "TIME", { { 0.0f, "-4s" }, { 0.5f, "-2s" }, { 1.0f, "NOW" } } },
             { "CUTOFF Hz", { { valueFor (100.0f), "100" }, { valueFor (1000.0f), "1k" },
                              { valueFor (10000.0f), "10k" } } },
             { "PAN", { { 0.0f, "L" }, { 1.0f, "R" } } });

    if (source)
        start();
}

float SquelchDisplay::valueFor (float hz) const
{
    return std::log (juce::jlimit (minHz, maxHz, hz) / minHz) / std::log (maxHz / minHz);
}

float SquelchDisplay::timeFor (std::int64_t sample) const
{
    const auto age = (float) ((double) (state.nowSample - sample) / state.sampleRate);
    return 1.0f - age / window;
}

void SquelchDisplay::tick (double)
{
    if (source)
        update (source());
}

void SquelchDisplay::step (const DisplayState& newState, double deltaSeconds)
{
    update (newState);
    renderFrame (deltaSeconds);
}

void SquelchDisplay::update (const DisplayState& newState)
{
    state = newState;

    if (seen && state.eventCount != lastEvents)
        flash();

    seen = true;
    lastEvents = state.eventCount;
    setTimeCursor (1.0f);

    juce::StringArray left, right;
    left.add (reactionNames[juce::jlimit (0, reactionNames.size() - 1, state.reaction)]);
    left.add (modeNames[juce::jlimit (0, modeNames.size() - 1, state.mode)]
              + (state.mode == 0 ? " " + gridNames[juce::jlimit (0, gridNames.size() - 1, state.gridIndex)]
                                 : juce::String()));

    right.add ("HELD " + hzText (state.heldHz) + " Hz");
    right.add ("VOICES " + juce::String (state.liveVoices));

    if (state.meltdown)
        right.add ("MELTDOWN");

    if (left != lastLeft || right != lastRight)
    {
        lastLeft = left;
        lastRight = right;
        setReadouts (left, right);
    }
}

void SquelchDisplay::drawFrame (juce::Graphics& g, const vis::Iso& iso, double)
{
    if (state.nowSample <= 0)
        return;

    const auto line = color::visualizer::line;
    const auto thin = sps::tokens::visualizer::lineWidth;
    const auto strong = sps::tokens::visualizer::lineWidthStrong;

    // The scheduler's grid, as ticks along the floor at the near edge: filled where the step
    // passed the probability gate, hollow where it did not, so a gap is visible.
    const auto steps = (int) juce::jmin ((unsigned) DisplayState::maxSteps, state.stepCount);

    for (int i = 0; i < steps; ++i)
    {
        const auto& step = state.steps[(state.stepCount - 1u - (unsigned) i) % (unsigned) DisplayState::maxSteps];
        const auto t = timeFor (step.start);

        if (t < 0.0f || t > 1.0f)
            continue;

        const auto foot = iso (t, 0.0f, 0.0f);
        const auto top = iso (t, step.fired ? 0.06f : 0.03f, 0.0f);

        g.setColour (line.withAlpha (step.fired ? 0.9f : 0.35f));
        g.drawLine (foot.x, foot.y, top.x, top.y, thin);
    }

    // Each event, oldest first so the newest is drawn last.
    const auto events = (int) juce::jmin ((unsigned) DisplayState::maxEvents, state.eventCount);

    for (int i = events - 1; i >= 0; --i)
    {
        const auto& e = state.events[(state.eventCount - 1u - (unsigned) i) % (unsigned) DisplayState::maxEvents];
        const auto age = (float) ((double) (state.nowSample - e.start) / state.sampleRate);

        if (age < 0.0f || age > window)
            continue;

        const auto depth = juce::jlimit (0.0f, 1.0f, 0.5f + 0.5f * e.pan);

        // The sweep is e^(-u/tau): five time constants is as long as it is distinguishable
        // from the held cutoff, or until now if the event is younger than that.
        const auto length = juce::jmin (age, 5.0f * e.tauSeconds);

        for (int k = 0; k < curvePoints; ++k)
        {
            const auto u0 = length * (float) k / (float) curvePoints;
            const auto u1 = length * (float) (k + 1) / (float) curvePoints;

            const auto octAt = [&e] (float u)
            {
                auto oct = e.restOct + (e.peakOct - e.restOct) * std::exp (-u / e.tauSeconds);

                if (e.slide && u < e.slideSeconds)
                {
                    const auto t = u / juce::jmax (1.0e-4f, e.slideSeconds);
                    oct = e.slideFromOct * (1.0f - t) + oct * t;
                }

                return oct;
            };

            const auto o0 = octAt (u0);
            const auto o1 = octAt (u1);
            const auto a = iso (timeFor (e.start) + u0 / window, valueFor (std::exp2 (o0)), depth);
            const auto b = iso (timeFor (e.start) + u1 / window, valueFor (std::exp2 (o1)), depth);

            // Where the curve is above the cutoff at which feedback reaches self-oscillation,
            // the filter is ringing on its own: that part is drawn strong.
            const auto ringing = e.ringOct >= 0.0f && o0 >= e.ringOct;

            g.setColour (line.withAlpha (juce::jlimit (0.2f, 1.0f, e.intensity * (e.accent ? 1.0f : 0.75f))));
            g.drawLine (a.x, a.y, b.x, b.y, ringing ? strong : thin);
        }
    }
}

void SquelchDisplay::drawStage (juce::Graphics& g, const vis::Iso& iso)
{
    sps::VisualizerPanel::drawStage (g, iso);

    if (state.nowSample <= 0)
        return;

    vis::drawLevel (g, iso, valueFor (state.heldHz), "HELD", 0.5f);
}
} // namespace squelch
