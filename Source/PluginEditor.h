#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Components/PresetToolbar.h"
#include "Components/RotaryKnob.h"
#include "Components/SwitchSelector.h"
#include "Components/Toggle.h"
#include "PluginProcessor.h"

class SquelchAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit SquelchAudioProcessorEditor (SquelchAudioProcessor&);
    ~SquelchAudioProcessorEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Knob
    {
        sps::RotaryKnob control { sps::RotaryKnob::Style::Default };
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    void addKnob (const squelch::Continuous& spec);

    SquelchAudioProcessor& processor;

    sps::PresetToolbar toolbar;
    juce::TooltipWindow tooltips { this, 500 };

    std::vector<std::unique_ptr<Knob>> knobs;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SquelchAudioProcessorEditor)
};
