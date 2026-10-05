#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Components/Adjustor.h"
#include "Components/BackgroundFrame.h"
#include "Components/PresetToolbar.h"
#include "Components/SimpleKnob.h"
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
        sps::SimpleKnob control { sps::SimpleKnob::Style::Minimal };
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    /// A titled panel and its controls, in the order the cheatsheet lists them: stacked down
    /// a column, or laid along a row.
    struct Section
    {
        sps::BackgroundFrame frame;
        std::vector<juce::Component*> controls;
        bool row = false;

        /// Knobs per row in a column section.
        int columns = 1;
    };

    enum SectionIndex { structure, voice, colour, gestures, numSections };

    /// Widths of the outer and middle columns, and the height they all share.
    struct Metrics { int outer, centre, tallest; };

    Metrics measure() const;
    static void layOut (Section&, juce::Rectangle<int> frameArea);

    void addKnob (const char* id, Section& section);
    void addToggle (sps::Toggle& toggle, const char* id, const char* label,
                    const char* tooltip, Section& section,
                    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>& attachment);
    void addSelector (sps::SwitchSelector& selector, const char* id, const char* label,
                      const char* tooltip, const juce::StringArray& options,
                      Section* section, std::unique_ptr<juce::ParameterAttachment>& attachment);

    SquelchAudioProcessor& processor;

    sps::PresetToolbar toolbar;
    juce::TooltipWindow tooltips { this, 500 };

    std::array<Section, numSections> sections;

    /// The one control that is not in a section: it sits between VOICE and COLOUR.
    sps::SwitchSelector reaction { sps::SwitchSelector::Type::Symbol,
                                   squelch::reactionNames.size(),
                                   sps::SwitchSelector::Knob::Primary };
    sps::SwitchSelector mode { sps::SwitchSelector::Type::Text, squelch::modeNames.size() };
    sps::Adjustor seed { sps::Adjustor::Usage::Seed };
    sps::Toggle ionize, meltdown, clip;

    std::unique_ptr<juce::ParameterAttachment> reactionAttachment, modeAttachment,
                                               gridAttachment, seedAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        ionizeAttachment, meltdownAttachment, clipAttachment;

    /// SwitchSelector::setPosition notifies synchronously, so a parameter-driven
    /// update would otherwise echo straight back out as a new gesture.
    bool updatingFromParameter = false;

    std::vector<std::unique_ptr<Knob>> knobs;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SquelchAudioProcessorEditor)
};
