#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "DisplayState.h"

namespace squelch
{
/** A cloud chamber. Every event is a bullet fired into the chamber from the left, and its track
    is the filter's cutoff sweep drawn in the chamber's own coordinates: height is the cutoff in
    Hz, so a steep fall is a fast decay and a long shallow arc is a slow one. What the track does
    on the way is the reaction: RADIATION forks, FISSION splits, SLUDGE coils, CHEMICAL scatters,
    ALIEN loops back on itself. Tracks fade as the vapour disperses.

    Everything drawn is a value the audio thread published. See visualizer.md. */
class SquelchDisplay final : public juce::Component,
                             private juce::Timer
{
public:
    static constexpr int designWidth = 880;
    static constexpr int designHeight = 340;

    /** `source` is polled on the message thread at 60 Hz. */
    explicit SquelchDisplay (std::function<DisplayState()> source);
    ~SquelchDisplay() override;

    void paint (juce::Graphics& g) override;

private:
    void timerCallback() override;

    std::function<DisplayState()> source;
    DisplayState state;
    unsigned lastEvents = 0;
    float flashAmount = 0.0f;
    bool seen = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SquelchDisplay)
};
} // namespace squelch
