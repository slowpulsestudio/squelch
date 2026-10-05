#include "PluginEditor.h"

#include "sps/Tokens.h"

namespace
{
    constexpr int margin = 16;
    constexpr int gap = 16;
    constexpr int padding = 16;

    /// A knob's grid cell, as in the design system's knob group: the 54px knob centred in
    /// 80x78, which leaves room for its name either side.
    constexpr int knobCellWidth = 80;
    constexpr int knobCellHeight = 78;

    /// Between controls stacked in a column. A knob's cell already carries its own margin.
    constexpr int stackGap = 8;

    juce::Rectangle<int> designSizeOf (juce::Component& c)
    {
        if (auto* selector = dynamic_cast<sps::SwitchSelector*> (&c))
            return selector->designBounds();

        if (auto* adjustor = dynamic_cast<sps::Adjustor*> (&c))
            return adjustor->designBounds();

        if (dynamic_cast<sps::Toggle*> (&c) != nullptr)
            return { sps::Toggle::designWidth, sps::Toggle::designHeight };

        if (auto* knob = dynamic_cast<sps::SimpleKnob*> (&c))
            return knob->designBounds();

        jassertfalse;
        return {};
    }

    /// The grid cell a control occupies. Knobs are narrower than theirs; the rest fill it.
    juce::Rectangle<int> cellSizeOf (juce::Component& c)
    {
        const auto size = designSizeOf (c);

        if (dynamic_cast<sps::SimpleKnob*> (&c) != nullptr)
            return { juce::jmax (size.getWidth(), knobCellWidth),
                     juce::jmax (size.getHeight(), knobCellHeight) };

        return size;
    }

    int columnWidth (const std::vector<juce::Component*>& controls)
    {
        auto width = 0;
        for (auto* c : controls)
            width = juce::jmax (width, cellSizeOf (*c).getWidth());
        return width + padding * 2;
    }

    int columnHeight (const std::vector<juce::Component*>& controls)
    {
        auto height = padding * 2;
        for (auto* c : controls)
            height += cellSizeOf (*c).getHeight();
        return height + stackGap * juce::jmax (0, (int) controls.size() - 1);
    }

    /// Each column is as wide as its mirror image about the centre, so the centrepiece sits
    /// exactly halfway across the plugin and not just between the two middle columns.
    template <typename Sections>
    int mirroredWidth (const Sections& sections, int index)
    {
        const auto mirror = (int) sections.size() - 1 - index;
        return juce::jmax (columnWidth (sections[(size_t) index].controls),
                           columnWidth (sections[(size_t) mirror].controls));
    }

    /// JUCE only asks the component directly under the mouse, so every child needs the text.
    void applyTooltip (juce::Component& c, const juce::String& text)
    {
        if (auto* tip = dynamic_cast<juce::SettableTooltipClient*> (&c))
            tip->setTooltip (text);

        for (int i = 0; i < c.getNumChildComponents(); ++i)
            applyTooltip (*c.getChildComponent (i), text);
    }

    const squelch::Continuous& specFor (const char* id)
    {
        for (const auto& spec : squelch::continuous)
            if (juce::String (spec.id) == id)
                return spec;

        jassertfalse;
        return squelch::continuous[0];
    }
}

SquelchAudioProcessorEditor::SquelchAudioProcessorEditor (SquelchAudioProcessor& p)
    : juce::AudioProcessorEditor (&p), processor (p)
{
    addAndMakeVisible (toolbar);

    // The frames go in first so the controls sit in front of them.
    const char* titles[numSections] { "STRUCTURE", "VOICE", "COLOUR", "GESTURES" };

    for (auto i = 0; i < numSections; ++i)
    {
        sections[i].frame.setTitle (titles[i]);
        addAndMakeVisible (sections[i].frame);
    }

    // Glyphs in the order of reactionNames, which is the order the detents run round.
    reaction.setIcons ({ sps::Icon::Type::Radiation, sps::Icon::Type::Fission,
                         sps::Icon::Type::Sludge, sps::Icon::Type::Beaker,
                         sps::Icon::Type::Alien });
    reaction.setBulb (true);
    addSelector (reaction, squelch::ids::reaction, "Reaction", squelch::reactionTooltip,
                 squelch::reactionNames, nullptr, reactionAttachment);

    // Figma's Adjustor property: the division the mode's positions are qualified by. The
    // name under the selector names the pair, as it does in the design system's own
    // selector-and-quartz example, so the division readout needs none of its own.
    mode.setAdjustor (true);
    addSelector (mode, squelch::ids::mode, "Timing", squelch::modeTooltip,
                 squelch::modeNames, &sections[structure], modeAttachment);

    if (auto* grid = mode.getAdjustor())
    {
        grid->setDivisions (squelch::gridNames);
        applyTooltip (*grid, squelch::gridTooltip);

        auto& parameter = *processor.apvts.getParameter (squelch::ids::grid);

        gridAttachment = std::make_unique<juce::ParameterAttachment> (
            parameter,
            [grid] (float value)
            {
                grid->setDivisionIndex ((int) value, juce::dontSendNotification);
            },
            processor.apvts.undoManager);

        grid->onDivisionChange = [this] (int index)
        {
            gridAttachment->setValueAsCompleteGesture ((float) index);
        };

        gridAttachment->sendInitialUpdate();
    }

    seed.setLabelText ("Seed");
    applyTooltip (seed, squelch::seedTooltip);
    {
        auto& parameter = *processor.apvts.getParameter (squelch::ids::seed);
        const auto range = parameter.getNormalisableRange();
        seed.setRange (range.start, range.end, 1.0);

        seedAttachment = std::make_unique<juce::ParameterAttachment> (
            parameter,
            [this] (float value)
            {
                seed.setValue (value, juce::dontSendNotification);

                // Adjustor::setValue returns early when the value has not moved, so the
                // readout keeps its placeholder at the default seed of 0 without this.
                seed.setValueText (juce::String (juce::roundToInt (value)));
            },
            processor.apvts.undoManager);

        seed.onValueChange = [this] (double value)
        {
            seedAttachment->setValueAsCompleteGesture ((float) value);
        };

        seedAttachment->sendInitialUpdate();
    }
    addAndMakeVisible (seed);
    sections[structure].controls.push_back (&seed);

    for (const auto* id : { squelch::ids::probability, squelch::ids::flux,
                            squelch::ids::volatility, squelch::ids::halfLife,
                            squelch::ids::containment })
        addKnob (id, sections[structure]);

    for (const auto* id : { squelch::ids::spread, squelch::ids::decay, squelch::ids::exposure,
                            squelch::ids::toxicity, squelch::ids::reactivity })
        addKnob (id, sections[voice]);

    for (const auto* id : { squelch::ids::enrichment, squelch::ids::contamination,
                            squelch::ids::drive, squelch::ids::collimator,
                            squelch::ids::fallout, squelch::ids::afterglow })
        addKnob (id, sections[colour]);

    addToggle (ionize, squelch::ids::ionize, "Ionize", squelch::ionizeTooltip,
               sections[gestures], ionizeAttachment);
    addKnob (squelch::ids::ionizeAmount, sections[gestures]);
    addToggle (meltdown, squelch::ids::meltdown, "Meltdown", squelch::meltdownTooltip,
               sections[gestures], meltdownAttachment);
    addToggle (clip, squelch::ids::clip, "Clip", squelch::clipTooltip,
               sections[gestures], clipAttachment);

    auto tallest = 0;
    auto width = margin * 2 + gap * numSections + reaction.designBounds().getWidth();
    for (auto i = 0; i < numSections; ++i)
    {
        tallest = juce::jmax (tallest, columnHeight (sections[i].controls));
        width += mirroredWidth (sections, i);
    }

    setSize (juce::jmax (width, margin * 2 + sps::PresetToolbar::designWidth),
             margin * 2 + sps::PresetToolbar::designHeight + gap
                 + sps::BackgroundFrame::titleOverhang + tallest);
}

void SquelchAudioProcessorEditor::addKnob (const char* id, Section& section)
{
    const auto& spec = specFor (id);

    auto knob = std::make_unique<Knob>();

    knob->control.setLabelText (spec.label);
    applyTooltip (knob->control, spec.tooltip);

    knob->attachment
        = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.apvts, spec.id, *knob->control.control());

    addAndMakeVisible (knob->control);
    section.controls.push_back (&knob->control);
    knobs.push_back (std::move (knob));
}

void SquelchAudioProcessorEditor::addToggle (
    sps::Toggle& toggle, const char* id, const char* label, const char* tooltip,
    Section& section,
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>& attachment)
{
    toggle.setLabelText (label);
    toggle.setTooltip (tooltip);

    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        processor.apvts, id, toggle);

    addAndMakeVisible (toggle);
    section.controls.push_back (&toggle);
}

void SquelchAudioProcessorEditor::addSelector (
    sps::SwitchSelector& selector, const char* id, const char* label, const char* tooltip,
    const juce::StringArray& options, Section* section,
    std::unique_ptr<juce::ParameterAttachment>& attachment)
{
    selector.setLabelText (label);
    selector.setOptions (options);
    applyTooltip (selector, tooltip);

    // The selector counts its detents from one and the choice parameter from zero,
    // so this cannot be a SliderAttachment: that would bind the two off by one.
    attachment = std::make_unique<juce::ParameterAttachment> (
        *processor.apvts.getParameter (id),
        [this, &selector] (float value)
        {
            const juce::ScopedValueSetter<bool> guard (updatingFromParameter, true);
            selector.setPosition ((int) value + 1);
        },
        processor.apvts.undoManager);

    selector.onPositionChange = [this, &attachment] (int position)
    {
        if (! updatingFromParameter)
            attachment->setValueAsCompleteGesture ((float) (position - 1));
    };

    attachment->sendInitialUpdate();

    addAndMakeVisible (selector);

    if (section != nullptr)
        section->controls.push_back (&selector);
}

void SquelchAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (sps::tokens::color::surface::panel);
}

void SquelchAudioProcessorEditor::layOut (Section& section, juce::Rectangle<int> frameArea)
{
    section.frame.setBounds (frameArea);

    // Controls stack down the column from the top, each centred across it.
    auto y = frameArea.getY() + padding;
    const auto centreX = frameArea.getCentreX();

    for (auto* c : section.controls)
    {
        const auto cell = cellSizeOf (*c);
        c->setBounds (designSizeOf (*c).withCentre ({ centreX, y + cell.getHeight() / 2 }));
        y += cell.getHeight() + stackGap;
    }
}

void SquelchAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (margin);

    toolbar.setBounds (area.removeFromTop (sps::PresetToolbar::designHeight)
                           .withWidth (sps::PresetToolbar::designWidth));
    area.removeFromTop (gap);

    // The title straddles the top edge, so the frames need room above them.
    area.removeFromTop (sps::BackgroundFrame::titleOverhang);

    for (auto i = 0; i < numSections; ++i)
    {
        layOut (sections[i], area.removeFromLeft (mirroredWidth (sections, i)));
        area.removeFromLeft (gap);

        // REACTION is the plugin's centrepiece: halfway through the sections, and
        // vertically centred in the height they share.
        if (i == voice)
        {
            const auto size = reaction.designBounds();
            reaction.setBounds (size.withCentre (area.removeFromLeft (size.getWidth()).getCentre()));
            area.removeFromLeft (gap);
        }
    }
}
