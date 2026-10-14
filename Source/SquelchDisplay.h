#pragma once

#include <functional>

#include "Components/VisualizerPanel.h"
#include "DisplayState.h"

namespace squelch
{
/** The sweeps as the shared 4D isometric display: cutoff up on a log axis, the last few
    seconds along the beam, and each event's stereo placement as depth. Each event is the
    closed-form curve the envelope built for it, plotted from the same constants the audio
    thread is playing, so the display and the sound cannot disagree about a sweep.

    Everything on it is a value the audio thread published. See visualizer.md.
*/
class SquelchDisplay final : public sps::VisualizerPanel
{
public:
    static constexpr int designHeight = 340;

    /** `source` is polled by the panel's clock; pass an empty function to drive it by hand. */
    explicit SquelchDisplay (std::function<DisplayState()> source);

    /** Feeds a state and draws one frame without a message loop, for a bench. */
    void step (const DisplayState& newState, double deltaSeconds);

protected:
    void tick (double deltaSeconds) override;
    void drawFrame (juce::Graphics& g, const sps::visualizer::Iso& iso, double deltaSeconds) override;
    void drawStage (juce::Graphics& g, const sps::visualizer::Iso& iso) override;

private:
    void update (const DisplayState& newState);
    float valueFor (float hz) const;
    float timeFor (std::int64_t sample) const;

    std::function<DisplayState()> source;
    DisplayState state;

    unsigned lastEvents = 0;
    bool seen = false;
    juce::StringArray lastLeft, lastRight;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SquelchDisplay)
};
} // namespace squelch
