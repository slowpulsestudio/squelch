#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Components/Adjustor.h"
#include "Components/BackgroundFrame.h"
#include "Components/InputButton.h"
#include "Components/Parameter.h"
#include "Components/PresetToolbar.h"
#include "Components/SimpleKnob.h"
#include "Components/SwitchSelector.h"
#include "Components/Toggle.h"
#include "PluginProcessor.h"
#include "SquelchDisplay.h"

/// A held gesture: the design system's momentary InputButton, with the parameter on while
/// it is down and off the instant it is released, wherever the pointer is.
class MomentaryGesture : public juce::Component
{
public:
    static constexpr int designWidth = sps::Toggle::designWidth;
    static constexpr int designHeight = sps::Toggle::designHeight;

    MomentaryGesture (juce::RangedAudioParameter&, const char* labelText, const char* tooltip);

    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::RangedAudioParameter& parameter;
    sps::InputButton button { sps::InputButton::Glyph::Alpha,
                              sps::InputButton::Interaction::Momentary };
    sps::Parameter label { sps::Parameter::Kind::Name };
};

class SquelchAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    private juce::AudioProcessorValueTreeState::Listener
{
public:
    explicit SquelchAudioProcessorEditor (SquelchAudioProcessor&);
    ~SquelchAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    sps::PresetToolbar& getToolbar() { return toolbar; }

private:
    void parameterChanged (const juce::String&, float) override;
    void selectPreset (int index);
    void randomise();
    struct Knob
    {
        sps::SimpleKnob control { sps::SimpleKnob::Style::Minimal };
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    /// A titled panel with a horizontal row or a wrapping grid and optional leading selector.
    struct Section
    {
        sps::BackgroundFrame frame;
        std::vector<juce::Component*> controls;
        bool row = false;

        /// Controls per row in a grid section.
        int columns = 1;
        juce::Component* leadingControl = nullptr;
    };

    enum SectionIndex { structure, voice, colour, gestures, output, numSections };

    struct Metrics { int width, height, topRow, bottomRow; };

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

    juce::Component content;
    sps::PresetToolbar toolbar;
    squelch::SquelchDisplay display;
    juce::TooltipWindow tooltips { this, 500 };

    std::array<Section, numSections> sections;

    /// The reaction selector sits beside the display.
    sps::SwitchSelector reaction { sps::SwitchSelector::Type::Symbol,
                                   squelch::reactionNames.size(),
                                   sps::SwitchSelector::Knob::Primary };
    sps::SwitchSelector mode { sps::SwitchSelector::Type::Text, squelch::modeNames.size() };
    sps::Adjustor seed { sps::Adjustor::Usage::Seed };
    sps::Toggle ionize, clip;
    MomentaryGesture meltdown;
    sps::SimpleKnob mix { sps::SimpleKnob::Style::Minimal };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mixAttachment;

    std::unique_ptr<juce::ParameterAttachment> reactionAttachment, modeAttachment,
                                               gridAttachment, seedAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        ionizeAttachment, clipAttachment;

    /// SwitchSelector::setPosition notifies synchronously, so a parameter-driven
    /// update would otherwise echo straight back out as a new gesture.
    bool updatingFromParameter = false;

    std::vector<std::unique_ptr<Knob>> knobs;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SquelchAudioProcessorEditor)
};
