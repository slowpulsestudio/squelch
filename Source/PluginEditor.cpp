#include "PluginEditor.h"

#include "sps/Tokens.h"

namespace
{
    constexpr int columns = 6;
    constexpr int margin = 16;
}

SquelchAudioProcessorEditor::SquelchAudioProcessorEditor (SquelchAudioProcessor& p)
    : juce::AudioProcessorEditor (&p), processor (p)
{
    addAndMakeVisible (toolbar);

    for (const auto& spec : squelch::continuous)
        addKnob (spec);

    const auto rows = (static_cast<int> (knobs.size()) + columns - 1) / columns;
    setSize (margin * 2 + columns * sps::RotaryKnob::designWidth,
             margin * 3 + sps::PresetToolbar::designHeight
                 + rows * sps::RotaryKnob::designHeight);
}

void SquelchAudioProcessorEditor::addKnob (const squelch::Continuous& spec)
{
    auto knob = std::make_unique<Knob>();

    knob->control.setLabelText (spec.label);
    knob->control.setTooltip (spec.tooltip);

    // TooltipWindow only looks at the component directly under the mouse and
    // does not walk up to the parent, so every child that takes the pointer
    // needs the text too.
    for (int i = 0; i < knob->control.getNumChildComponents(); ++i)
        if (auto* tip = dynamic_cast<juce::SettableTooltipClient*> (
                knob->control.getChildComponent (i)))
            tip->setTooltip (spec.tooltip);

    knob->attachment
        = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.apvts, spec.id, knob->control);

    addAndMakeVisible (knob->control);
    knobs.push_back (std::move (knob));
}

void SquelchAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (sps::tokens::color::surface::panel);
}

void SquelchAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (margin);

    toolbar.setBounds (area.removeFromTop (sps::PresetToolbar::designHeight)
                           .withWidth (sps::PresetToolbar::designWidth));
    area.removeFromTop (margin);

    for (size_t i = 0; i < knobs.size(); ++i)
    {
        const auto column = static_cast<int> (i) % columns;
        const auto row = static_cast<int> (i) / columns;

        knobs[i]->control.setBounds (area.getX() + column * sps::RotaryKnob::designWidth,
                                     area.getY() + row * sps::RotaryKnob::designHeight,
                                     sps::RotaryKnob::designWidth,
                                     sps::RotaryKnob::designHeight);
    }
}
