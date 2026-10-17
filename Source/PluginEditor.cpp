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
    constexpr int displayHeight = squelch::SquelchDisplay::designHeight;

    juce::Rectangle<int> designSizeOf (juce::Component& c)
    {
        if (auto* selector = dynamic_cast<sps::SwitchSelector*> (&c))
            return selector->designBounds();

        if (auto* adjustor = dynamic_cast<sps::Adjustor*> (&c))
            return adjustor->designBounds();

        if (dynamic_cast<sps::Toggle*> (&c) != nullptr)
            return { sps::Toggle::designWidth, sps::Toggle::designHeight };

        if (dynamic_cast<MomentaryGesture*> (&c) != nullptr)
            return { 0, 0, MomentaryGesture::designWidth, MomentaryGesture::designHeight };

        if (auto* knob = dynamic_cast<sps::SimpleKnob*> (&c))
            return knob->designBounds();

        jassertfalse;
        return {};
    }

    /// The grid cell a control occupies. Knobs are narrower than theirs; the rest fill it.
    juce::Rectangle<int> cellSizeOf (juce::Component& c)
    {
        const auto size = designSizeOf (c);

        if (dynamic_cast<sps::SimpleKnob*> (&c) != nullptr
            || dynamic_cast<sps::Adjustor*> (&c) != nullptr)
            return { juce::jmax (size.getWidth(), knobCellWidth),
                     juce::jmax (size.getHeight(), knobCellHeight) };

        return size;
    }

    using Row = std::vector<juce::Component*>;

    /// Horizontal sections use one row; grids wrap at their authored column count.
    template <typename Section>
    std::vector<Row> rowsOf (const Section& section)
    {
        if (section.row)
            return { section.controls };

        std::vector<Row> rows;

        for (auto* c : section.controls)
        {
            if (! rows.empty() && (int) rows.back().size() < section.columns)
                rows.back().push_back (c);
            else
                rows.push_back ({ c });
        }

        return rows;
    }

    int rowWidth (const Row& row)
    {
        auto width = 0;
        for (auto* c : row)
            width += cellSizeOf (*c).getWidth();
        return width;
    }

    int rowHeight (const Row& row)
    {
        auto height = 0;
        for (auto* c : row)
            height = juce::jmax (height, cellSizeOf (*c).getHeight());
        return height;
    }

    template <typename Section>
    int sectionWidth (const Section& section)
    {
        auto width = 0;
        for (const auto& row : rowsOf (section))
            width = juce::jmax (width, rowWidth (row));

        return width + padding * 2 + (section.leadingControl != nullptr
            ? designSizeOf (*section.leadingControl).getWidth() + gap : 0);
    }

    template <typename Section>
    int sectionHeight (const Section& section)
    {
        const auto rows = rowsOf (section);
        auto height = 0;

        for (const auto& row : rows)
            height += rowHeight (row);

        height += stackGap * juce::jmax (0, (int) rows.size() - 1);
        if (section.leadingControl != nullptr)
            height = juce::jmax (height, designSizeOf (*section.leadingControl).getHeight());
        return height + padding * 2;
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

MomentaryGesture::MomentaryGesture (juce::RangedAudioParameter& p, const char* labelText,
                                    const char* tooltip)
    : parameter (p)
{
    label.setText (labelText);
    label.setInterceptsMouseClicks (false, false);
    setInterceptsMouseClicks (false, true);

    // The button swallows its own clicks, so ask it to tell us about them.
    button.addMouseListener (this, false);
    applyTooltip (button, tooltip);

    addAndMakeVisible (button);
    addAndMakeVisible (label);
}

void MomentaryGesture::resized()
{
    // The label sits where the Toggle's does, so the gestures line up in the column.
    button.setBounds (juce::Rectangle<int> (sps::InputButton::designWidth,
                                            sps::InputButton::designHeight)
                          .withCentre ({ designWidth / 2, 70 }));
    label.setBounds (12, 110, 60, 28);
}

void MomentaryGesture::mouseDown (const juce::MouseEvent&)
{
    parameter.beginChangeGesture();
    parameter.setValueNotifyingHost (1.0f);
}

void MomentaryGesture::mouseUp (const juce::MouseEvent&)
{
    parameter.setValueNotifyingHost (0.0f);
    parameter.endChangeGesture();
}

SquelchAudioProcessorEditor::SquelchAudioProcessorEditor (SquelchAudioProcessor& p)
    : juce::AudioProcessorEditor (&p), processor (p),
      display ([&p] { return p.displayState(); }),
      meltdown (*p.apvts.getParameter (squelch::ids::meltdown), "Meltdown",
                squelch::meltdownTooltip)
{
    addAndMakeVisible (content);
    content.addAndMakeVisible (display);
    content.addAndMakeVisible (toolbar);

    // The frames go in first so the controls sit in front of them.
    const char* titles[numSections] { "STRUCTURE", "VOICE", "COLOUR", "GESTURES", "OUTPUT" };

    for (auto i = 0; i < numSections; ++i)
    {
        sections[i].frame.setTitle (titles[i]);
        content.addAndMakeVisible (sections[i].frame);
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
                 squelch::modeNames, nullptr, modeAttachment);
    sections[structure].leadingControl = &mode;

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
    content.addAndMakeVisible (seed);
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
    content.addAndMakeVisible (meltdown);
    sections[gestures].controls.push_back (&meltdown);
    addToggle (clip, squelch::ids::clip, "Clip", squelch::clipTooltip,
               sections[output], clipAttachment);
    mix.setLabelText ("Mix");
    mix.setComponentID (squelch::ids::mix);
    applyTooltip (mix, squelch::mixTooltip);
    mixAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.apvts, squelch::ids::mix, *mix.control());
    content.addAndMakeVisible (mix);
    sections[output].controls.insert (sections[output].controls.begin(), &mix);

    sections[voice].row = true;
    sections[colour].row = true;
    sections[gestures].row = true;
    sections[output].row = true;

    sections[structure].columns = 3;

    toolbar.setPresetNames (squelch::presets::names);
    toolbar.onPresetSelected = [this] (int index) { selectPreset (index); };
    toolbar.onRandomise = [this] { randomise(); };
    toolbar.isDirty = [this]
    {
        const auto chosen = processor.presetIndex.load();
        return chosen >= 0 && ! squelch::presets::matches (processor.apvts, chosen);
    };

    // A fresh instance opens on the first preset, through the path the toolbar uses.
    // A restored one, or a reopened editor, shows what was chosen.
    switch (const auto chosen = processor.presetIndex.load())
    {
        case squelch::presets::notChosen:
            selectPreset (0);
            toolbar.setSelectedPreset (0);
            break;
        case squelch::presets::randomised:
            toolbar.showUnsavedLabel ("RANDOM");
            break;
        default:
            toolbar.setSelectedPreset (chosen);
            break;
    }

    squelch::presets::forEachParameter (processor.apvts, [this] (juce::RangedAudioParameter& p)
                                        { processor.apvts.addParameterListener (p.paramID, this); });

    const auto m = measure();

    auto scale = 1.0f;
    if (const auto* screen = juce::Desktop::getInstance().getDisplays().getDisplayForPoint (
            juce::Desktop::getMousePosition().toFloat()))
    {
        const auto available = screen->userBounds.reduced (24.0f);
        scale = juce::jmin (scale, available.getWidth() / (float) m.width,
                                  available.getHeight() / (float) m.height);
    }
    setSize (juce::roundToInt ((float) m.width * scale),
             juce::roundToInt ((float) m.height * scale));
}

SquelchAudioProcessorEditor::~SquelchAudioProcessorEditor()
{
    squelch::presets::forEachParameter (processor.apvts, [this] (juce::RangedAudioParameter& p)
                                        { processor.apvts.removeParameterListener (p.paramID, this); });
}

void SquelchAudioProcessorEditor::selectPreset (int index)
{
    processor.presetIndex = index;
    squelch::presets::apply (processor.apvts, index);
    toolbar.refreshDisplay();
}

void SquelchAudioProcessorEditor::randomise()
{
    processor.presetIndex = squelch::presets::randomised;
    squelch::presets::randomise (processor.apvts);
}

void SquelchAudioProcessorEditor::parameterChanged (const juce::String&, float)
{
    // Can arrive from the audio thread, as host automation does.
    juce::Component::SafePointer<SquelchAudioProcessorEditor> self (this);
    juce::MessageManager::callAsync ([self] { if (self != nullptr) self->toolbar.refreshDisplay(); });
}

SquelchAudioProcessorEditor::Metrics SquelchAudioProcessorEditor::measure() const
{
    Metrics m;
    m.width = margin * 2 + juce::jmax (
        sectionWidth (sections[structure]) + gap + sectionWidth (sections[gestures])
            + gap + sectionWidth (sections[output]),
        sectionWidth (sections[voice]) + gap + sectionWidth (sections[colour]));
    m.topRow = juce::jmax (sectionHeight (sections[structure]),
                           sectionHeight (sections[gestures]), sectionHeight (sections[output]));
    m.bottomRow = juce::jmax (sectionHeight (sections[voice]), sectionHeight (sections[colour]));
    m.height = margin * 2 + sps::PresetToolbar::designHeight + gap + displayHeight + gap
        + sps::BackgroundFrame::titleOverhang + m.topRow + gap
        + sps::BackgroundFrame::titleOverhang + m.bottomRow;
    return m;
}

void SquelchAudioProcessorEditor::addKnob (const char* id, Section& section)
{
    const auto& spec = specFor (id);

    auto knob = std::make_unique<Knob>();

    knob->control.setLabelText (spec.shortLabel != nullptr ? spec.shortLabel : spec.label);
    applyTooltip (knob->control, spec.tooltip);

    knob->attachment
        = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            processor.apvts, spec.id, *knob->control.control());

    content.addAndMakeVisible (knob->control);
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

    content.addAndMakeVisible (toggle);
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

    content.addAndMakeVisible (selector);

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

    const auto rows = rowsOf (section);
    if (section.leadingControl != nullptr)
    {
        const auto size = designSizeOf (*section.leadingControl);
        section.leadingControl->setBounds (size.withCentre (
            { frameArea.getX() + padding + size.getWidth() / 2, frameArea.getCentreY() }));
        frameArea.removeFromLeft (size.getWidth() + gap);
    }
    const auto centreX = frameArea.getCentreX();

    // A row section sits in the middle of its frame; a column section hangs from the top.
    auto y = section.row ? frameArea.getCentreY() - (sectionHeight (section) - padding * 2) / 2
                         : frameArea.getY() + padding;

    for (const auto& row : rows)
    {
        // Short grid rows stay under the first column rather than drifting to the middle.
        const auto span = ! section.row
                            ? section.columns * knobCellWidth : rowWidth (row);
        auto x = centreX - span / 2;
        const auto height = rowHeight (row);

        for (auto* c : row)
        {
            const auto cell = cellSizeOf (*c);
            c->setBounds (designSizeOf (*c).withCentre ({ x + cell.getWidth() / 2,
                                                          y + height / 2 }));
            x += cell.getWidth();
        }

        y += height + stackGap;
    }
}

void SquelchAudioProcessorEditor::resized()
{
    const auto m = measure();
    content.setBounds (0, 0, m.width, m.height);
    const auto scale = juce::jmin ((float) getWidth() / (float) m.width,
                                  (float) getHeight() / (float) m.height);
    content.setTransform (juce::AffineTransform::scale (scale));
    auto area = content.getLocalBounds().reduced (margin);

    toolbar.setBounds (area.removeFromTop (sps::PresetToolbar::designHeight)
                           .withWidth (sps::PresetToolbar::designWidth));
    area.removeFromTop (gap);

    auto displayRow = area.removeFromTop (displayHeight);
    reaction.setBounds (reaction.designBounds().withCentre (
        displayRow.removeFromLeft (reaction.designBounds().getWidth()).getCentre()));
    displayRow.removeFromLeft (gap);
    display.setBounds (displayRow);
    area.removeFromTop (gap);

    // The title straddles the top edge, so the frames need room above them.
    area.removeFromTop (sps::BackgroundFrame::titleOverhang);

    auto top = area.removeFromTop (m.topRow);
    layOut (sections[structure], top.removeFromLeft (sectionWidth (sections[structure])));
    top.removeFromLeft (gap);
    layOut (sections[gestures], top.removeFromLeft (sectionWidth (sections[gestures])));
    top.removeFromLeft (gap);
    layOut (sections[output], top);

    area.removeFromTop (gap + sps::BackgroundFrame::titleOverhang);
    auto bottom = area.removeFromTop (m.bottomRow);
    layOut (sections[voice], bottom.removeFromLeft (sectionWidth (sections[voice])));
    bottom.removeFromLeft (gap);
    layOut (sections[colour], bottom);
}
