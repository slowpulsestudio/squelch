#include "SquelchDisplay.h"

#include <cmath>
#include <sps/Tokens.h>

#include "Components/Fonts.h"
#include "Parameters.h"

namespace squelch
{
namespace
{
    namespace color = sps::tokens::color;

    constexpr float minHz = 40.0f;
    constexpr float maxHz = 20000.0f;

    // How long a track stays on screen.
    constexpr float lifeSeconds = 3.5f;

    constexpr float pi = juce::MathConstants<float>::pi;

    float hash01 (std::uint64_t n)
    {
        n ^= n >> 33;
        n *= 0xff51afd7ed558ccdULL;
        n ^= n >> 33;
        n *= 0xc4ceb9fe1a85ec53ULL;
        n ^= n >> 33;
        return (float) (n & 0xffffffULL) / 16777216.0f;
    }

    juce::String hzText (float hz)
    {
        if (hz >= 10000.0f)
            return juce::String (juce::roundToInt (hz / 1000.0f)) + "k";

        if (hz >= 1000.0f)
            return juce::String (hz / 1000.0f, 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + "k";

        return juce::String (juce::roundToInt (hz));
    }
}

SquelchDisplay::SquelchDisplay (std::function<DisplayState()> sourceIn) : source (std::move (sourceIn))
{
    setOpaque (true);
    startTimerHz (60);
}

SquelchDisplay::~SquelchDisplay() = default;

void SquelchDisplay::timerCallback()
{
    if (source)
    {
        state = source();

        if (seen && state.eventCount != lastEvents)
            flashAmount = 1.0f;

        lastEvents = state.eventCount;
        seen = true;
    }

    flashAmount = juce::jmax (0.0f, flashAmount - 0.06f);
    repaint();
}

void SquelchDisplay::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto line = color::visualizer::line;
    const auto thin = sps::tokens::visualizer::lineWidth;
    const auto strong = sps::tokens::visualizer::lineWidthStrong;

    g.fillAll (color::surface::display);

    // The chamber, with room outside it for the cutoff scale.
    const auto chamber = bounds.reduced (12.0f).withTrimmedLeft (22.0f).withTrimmedRight (40.0f).withTrimmedTop (22.0f);
    const auto yFor = [&] (float hz)
    {
        const auto v = std::log (juce::jlimit (minHz, maxHz, hz) / minHz) / std::log (maxHz / minHz);
        return chamber.getBottom() - v * chamber.getHeight();
    };

    // Resting vapour, so the chamber is never empty.
    g.setColour (color::visualizer::grid);
    for (int i = 0; i < 90; ++i)
    {
        const auto x = chamber.getX() + hash01 ((std::uint64_t) i * 2u) * chamber.getWidth();
        const auto y = chamber.getY() + hash01 ((std::uint64_t) i * 2u + 1u) * chamber.getHeight();
        g.fillRect (x, y, 1.0f, 1.0f);
    }

    // The cutoff scale, down the right edge.
    g.setFont (sps::Fonts::geistMono (9.0f));
    for (const float hz : { 100.0f, 1000.0f, 10000.0f })
    {
        const auto y = yFor (hz);
        g.setColour (color::visualizer::grid);
        g.drawLine (chamber.getX(), y, chamber.getRight(), y, thin);
        g.setColour (color::visualizer::label);
        g.drawText (hzText (hz), juce::Rectangle<float> (chamber.getRight() + 4.0f, y - 6.0f, 36.0f, 12.0f),
                    juce::Justification::centredLeft, false);
    }

    // IONIZE leaves a standing haze, and MELTDOWN lights the whole chamber's edge.
    if (state.ionize)
    {
        g.setColour (line.withAlpha (0.16f));
        for (int i = 0; i < 140; ++i)
            g.fillRect (chamber.getX() + hash01 ((std::uint64_t) i * 3u + 900u) * chamber.getWidth(),
                        chamber.getY() + hash01 ((std::uint64_t) i * 3u + 901u) * chamber.getHeight(), 1.0f, 1.0f);
    }

    g.setColour (state.meltdown ? line : (flashAmount > 0.0f ? line.withAlpha (juce::jmax (0.35f, flashAmount)) : color::border::disabled));
    g.drawRect (chamber, state.meltdown ? strong : thin);

    // The held cutoff the sweeps come to rest on.
    g.setColour (color::visualizer::label.withAlpha (0.6f));
    g.drawLine (chamber.getX(), yFor (state.heldHz), chamber.getRight(), yFor (state.heldHz), thin);
    g.drawText ("HELD " + hzText (state.heldHz), juce::Rectangle<float> (chamber.getRight() - 90.0f, yFor (state.heldHz) - 13.0f, 86.0f, 12.0f),
                juce::Justification::centredRight, false);

    // The grid the scheduler runs on: ticks down the entry edge, newest at the top. A step
    // that did not pass the probability gate is a short tick and fires nothing.
    const auto steps = (int) juce::jmin ((unsigned) DisplayState::maxSteps, state.stepCount);
    for (int i = 0; i < steps; ++i)
    {
        const auto& step = state.steps[(state.stepCount - 1u - (unsigned) i) % (unsigned) DisplayState::maxSteps];
        const auto age = (float) ((double) (state.nowSample - step.start) / state.sampleRate);

        if (age < 0.0f || age > lifeSeconds)
            continue;

        const auto y = chamber.getY() + (age / lifeSeconds) * chamber.getHeight();
        g.setColour (line.withAlpha (step.fired ? 0.9f : 0.3f));
        g.drawLine (chamber.getX(), y, chamber.getX() + (step.fired ? 9.0f : 4.0f), y, thin);
    }

    // The bullets, oldest first so the newest is drawn on top.
    const auto events = (int) juce::jmin ((unsigned) DisplayState::maxEvents, state.eventCount);

    for (int i = events - 1; i >= 0; --i)
    {
        const auto& e = state.events[(state.eventCount - 1u - (unsigned) i) % (unsigned) DisplayState::maxEvents];
        const auto age = (float) ((double) (state.nowSample - e.start) / state.sampleRate);

        if (age < 0.0f || age > lifeSeconds)
            continue;

        // The sweep is rest + (peak - rest) e^(-t/tau): five time constants is as long as it
        // can be told from the held cutoff, and that is the track's whole length.
        const auto sweep = juce::jmax (0.05f, 5.0f * e.tauSeconds);
        const auto reach = juce::jlimit (0.3f, 0.95f, 0.3f + sweep / 1.6f) * chamber.getWidth();
        const auto lane = chamber.getX() + 6.0f + (0.5f + 0.5f * e.pan) * 0.1f * chamber.getWidth();

        // The bullet travels at a steady speed along the track and the track stays behind it.
        const auto travel = juce::jlimit (0.0f, 1.0f, age / juce::jmin (sweep, 1.2f));
        const auto fade = juce::jlimit (0.0f, 1.0f, 1.0f - age / lifeSeconds);
        const auto strength = e.intensity * (e.accent ? 1.0f : 0.75f);
        const auto seed = (std::uint64_t) e.start;

        const auto octAt = [&] (float u)
        {
            const auto t = u * sweep;
            auto oct = e.restOct + (e.peakOct - e.restOct) * std::exp (-t / juce::jmax (1.0e-3f, e.tauSeconds));

            if (e.slide && t < e.slideSeconds)
            {
                const auto k = t / juce::jmax (1.0e-4f, e.slideSeconds);
                oct = e.slideFromOct * (1.0f - k) + oct * k;
            }

            return oct;
        };

        // The track as a polyline in chamber coordinates, with the reaction's own behaviour added.
        constexpr int points = 64;
        const auto reaction = juce::jlimit (0, 4, state.reaction);
        std::vector<juce::Point<float>> track;
        track.reserve (points + 1);

        for (int k = 0; k <= points; ++k)
        {
            const auto u = (float) k / (float) points;
            auto x = lane + u * reach;
            auto y = yFor (std::exp2 (octAt (u)));

            if (reaction == 2) // SLUDGE coils as it slows
                y += 9.0f * (1.0f - u) * std::sin (u * 2.0f * pi * 7.0f);
            else if (reaction == 3) // CHEMICAL is knocked off course at a few fixed points
                for (int knock = 0; knock < 4; ++knock)
                    if (u > 0.15f + 0.2f * (float) knock)
                        y += (hash01 (seed + (std::uint64_t) knock) - 0.5f) * 2.0f * 10.0f * juce::jmin (1.0f, (u - 0.15f - 0.2f * (float) knock) * 6.0f) * 0.4f;
            else if (reaction == 4) // ALIEN loops back on itself midway
            {
                const auto loop = juce::jlimit (0.0f, 1.0f, (u - 0.4f) / 0.2f);
                x -= std::sin (loop * pi) * 26.0f;
                y -= (1.0f - std::cos (loop * 2.0f * pi)) * 10.0f;
            }

            track.push_back ({ x, juce::jlimit (chamber.getY(), chamber.getBottom(), y) });
        }

        const auto drawTrack = [&] (const std::vector<juce::Point<float>>& pts, float upTo, float alpha, float width, bool ring)
        {
            const auto last = (int) (upTo * (float) (pts.size() - 1));

            for (int k = 0; k < last; ++k)
            {
                // Above the cutoff where the filter rings on its own the track ionises and thickens.
                const auto u = (float) k / (float) (pts.size() - 1);
                const auto ringing = ring && e.ringOct >= 0.0f && octAt (u) >= e.ringOct;

                g.setColour (line.withAlpha (juce::jlimit (0.0f, 1.0f, alpha)));
                g.drawLine (pts[(size_t) k].x, pts[(size_t) k].y, pts[(size_t) k + 1].x, pts[(size_t) k + 1].y,
                            ringing ? strong : width);

                if (ringing)
                {
                    g.setColour (line.withAlpha (juce::jlimit (0.0f, 1.0f, alpha * 0.4f)));
                    g.drawLine (pts[(size_t) k].x, pts[(size_t) k].y - 2.5f, pts[(size_t) k + 1].x, pts[(size_t) k + 1].y - 2.5f, thin);
                    g.drawLine (pts[(size_t) k].x, pts[(size_t) k].y + 2.5f, pts[(size_t) k + 1].x, pts[(size_t) k + 1].y + 2.5f, thin);
                }
            }
        };

        drawTrack (track, travel, fade * strength, thin, true);

        // Forks and splits leave the main track at fixed fractions of its length.
        const auto branch = [&] (float from, float angle, float length, float alphaScale)
        {
            const auto at = track[(size_t) (from * (float) points)];
            const auto before = track[(size_t) juce::jmax (0, (int) (from * (float) points) - 2)];
            const auto heading = std::atan2 (at.y - before.y, at.x - before.x) + angle;
            std::vector<juce::Point<float>> pts;

            for (int k = 0; k <= 16; ++k)
                pts.push_back ({ at.x + std::cos (heading) * length * (float) k / 16.0f,
                                 juce::jlimit (chamber.getY(), chamber.getBottom(), at.y + std::sin (heading) * length * (float) k / 16.0f) });

            drawTrack (pts, juce::jlimit (0.0f, 1.0f, (travel - from) * 3.0f), fade * strength * alphaScale, thin, false);
        };

        if (reaction == 0)
        {
            branch (0.30f, 0.5f * (hash01 (seed + 11u) - 0.5f) * 2.0f + 0.4f, 0.18f * reach, 0.6f);
            branch (0.55f, -0.5f * hash01 (seed + 12u) - 0.2f, 0.14f * reach, 0.45f);
        }
        else if (reaction == 1)
        {
            branch (0.5f, 0.45f, 0.4f * reach, 0.9f);
            branch (0.5f, -0.45f, 0.4f * reach, 0.9f);
        }

        // The bullet's head.
        if (travel < 1.0f)
        {
            const auto head = track[(size_t) (travel * (float) points)];
            g.setColour (line.withAlpha (fade));
            g.drawLine (head.x - 3.0f, head.y, head.x + 3.0f, head.y, strong);
            g.drawLine (head.x, head.y - 3.0f, head.x, head.y + 3.0f, strong);
        }
    }

    // Titles and readouts.
    g.setFont (sps::Fonts::geistMono (10.0f, true));
    g.setColour (color::visualizer::label);
    g.drawText ("SQUELCH", juce::Rectangle<float> (14.0f, 4.0f, 200.0f, 14.0f), juce::Justification::centredLeft, false);

    g.setFont (sps::Fonts::geistMono (9.0f));
    const auto reactionName = reactionNames[juce::jlimit (0, reactionNames.size() - 1, state.reaction)];
    g.drawText (reactionName + "  " + modeNames[juce::jlimit (0, modeNames.size() - 1, state.mode)]
                    + (state.mode == 0 ? " " + gridNames[juce::jlimit (0, gridNames.size() - 1, state.gridIndex)] : juce::String()),
                juce::Rectangle<float> (120.0f, 4.0f, 300.0f, 14.0f), juce::Justification::centredLeft, false);
    g.drawText ("VOICES " + juce::String (state.liveVoices) + (state.meltdown ? "   MELTDOWN" : ""),
                juce::Rectangle<float> (bounds.getRight() - 214.0f, 4.0f, 200.0f, 14.0f), juce::Justification::centredRight, false);

    g.setColour (color::border::disabled);
    g.drawRoundedRectangle (bounds.reduced (0.5f), sps::tokens::radius::md, thin);
}
} // namespace squelch
