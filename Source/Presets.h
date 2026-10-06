#pragma once

#include <iterator>
#include <random>

#include <juce_audio_processors/juce_audio_processors.h>

#include "Parameters.h"

/** The presets, and Randomise.

    Both act on the same set of parameters, taken from the APVTS and filtered by
    isExcludedFromPresets, so neither can disagree with the layout. The Input and
    Output strips are not parameters and are never reached.
*/
namespace squelch::presets
{
    /// `presetIndex` before anything has been chosen, and after Randomise.
    inline constexpr int notChosen = -2;
    inline constexpr int randomised = -1;

    /// "Default" is first and is the layout's own defaults; the rest are rows of `rows`.
    inline const juce::StringArray names { "Default", "Isotope", "Neutron", "Plutonium", "Cesium", "Strontium", "Radon", "Uranium", "Thorium", "Tritium", "Krypton", "Xenon", "Polonium" };

    /// The parameters a preset stores, and the only ones. Every parameter that
    /// forEachParameter visits must be here, and a test says so.
    inline constexpr const char* columns[]
    {
        ids::reaction, ids::mode, ids::grid, ids::enrichment, ids::flux, ids::probability, ids::reactivity, ids::volatility, ids::halfLife, ids::decay, ids::spread, ids::toxicity, ids::containment, ids::drive, ids::contamination, ids::exposure, ids::collimator, ids::fallout, ids::afterglow, ids::seed
    };

    /** Twelve random presets, drawn once from a fixed seed and kept: not regenerated at
        run time. Values are normalised, in the order of `columns`. The names are placeholders. */
    inline constexpr float rows[][std::size (columns)]
    {
        { 0.75f, 1.0f, 0.3571f, 0.99f, 0.22f, 0.55f, 0.21f, 0.59f, 0.27f, 0.89f, 0.91f, 0.16f, 0.31f, 0.9f, 0.82f, 0.57f, 1.0f, 0.03f, 0.9f, 0.3223f },  // Isotope
        { 0.75f, 0.6667f, 0.2857f, 0.55f, 0.31f, 0.23f, 0.6f, 0.62f, 0.79f, 0.83f, 0.86f, 0.86f, 0.86f, 0.28f, 0.5f, 0.4f, 0.4f, 0.02f, 0.03f, 0.3243f },  // Neutron
        { 0.5f, 0.3333f, 0.4286f, 0.86f, 0.13f, 0.05f, 0.59f, 1.0f, 0.44f, 0.02f, 0.05f, 0.85f, 0.89f, 0.93f, 0.69f, 0.67f, 0.28f, 0.01f, 0.98f, 0.2112f },  // Plutonium
        { 0.5f, 0.0f, 0.5714f, 0.74f, 0.67f, 0.53f, 0.61f, 0.08f, 0.45f, 0.22f, 0.6f, 0.04f, 0.88f, 0.04f, 0.73f, 0.12f, 0.35f, 0.12f, 0.41f, 0.1682f },  // Cesium
        { 0.25f, 0.6667f, 0.6429f, 0.84f, 0.8f, 0.04f, 0.68f, 0.86f, 0.38f, 0.62f, 0.1f, 0.06f, 0.11f, 0.59f, 0.87f, 0.77f, 0.37f, 0.69f, 0.29f, 0.6897f },  // Strontium
        { 0.75f, 0.0f, 0.2143f, 0.86f, 0.44f, 0.72f, 0.49f, 0.89f, 0.9f, 0.06f, 0.28f, 0.18f, 0.35f, 0.28f, 0.94f, 0.49f, 0.07f, 0.16f, 0.2f, 0.4815f },  // Radon
        { 1.0f, 0.6667f, 0.6429f, 0.31f, 0.84f, 0.56f, 0.89f, 0.95f, 0.92f, 0.68f, 0.24f, 0.16f, 0.45f, 0.85f, 0.88f, 0.24f, 0.58f, 0.15f, 0.16f, 0.971f },  // Uranium
        { 0.0f, 0.3333f, 1.0f, 0.54f, 0.93f, 0.32f, 0.71f, 0.1f, 0.66f, 0.57f, 0.78f, 0.83f, 0.18f, 0.74f, 0.34f, 0.57f, 0.55f, 0.36f, 0.45f, 0.1451f },  // Thorium
        { 1.0f, 0.0f, 0.7857f, 0.9f, 0.82f, 0.69f, 0.89f, 0.0f, 0.86f, 0.16f, 0.91f, 0.89f, 0.1f, 0.71f, 0.09f, 0.67f, 0.21f, 0.21f, 0.32f, 0.981f },  // Tritium
        { 0.25f, 0.3333f, 0.7857f, 0.15f, 0.31f, 0.92f, 0.4f, 0.2f, 0.71f, 0.8f, 0.25f, 0.74f, 0.42f, 0.8f, 0.92f, 0.2f, 0.16f, 0.08f, 0.23f, 0.2893f },  // Krypton
        { 0.25f, 1.0f, 0.2143f, 0.98f, 0.85f, 0.94f, 0.3f, 0.17f, 0.26f, 0.47f, 0.67f, 0.01f, 0.94f, 0.75f, 0.71f, 0.65f, 0.34f, 0.06f, 0.51f, 0.8018f },  // Xenon
        { 0.25f, 0.6667f, 0.5714f, 0.34f, 0.76f, 0.82f, 0.34f, 0.56f, 0.0f, 0.15f, 0.29f, 0.11f, 0.17f, 0.98f, 0.24f, 0.04f, 0.79f, 0.27f, 0.9f, 0.7177f },  // Polonium
    };

    /// A parameter a preset does not name keeps its default.
    inline float valueFor (int preset, const juce::RangedAudioParameter& parameter)
    {
        if (preset > 0)
            for (size_t i = 0; i < std::size (columns); ++i)
                if (parameter.paramID == columns[i])
                    return rows[static_cast<size_t> (preset - 1)][i];

        return parameter.getDefaultValue();
    }

    template <typename Fn>
    void forEachParameter (juce::AudioProcessorValueTreeState& apvts, Fn&& fn)
    {
        for (auto* p : apvts.processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
                if (! isExcludedFromPresets (ranged->paramID))
                    fn (*ranged);
    }

    inline void set (juce::RangedAudioParameter& parameter, float normalised)
    {
        parameter.beginChangeGesture();
        parameter.setValueNotifyingHost (normalised);
        parameter.endChangeGesture();
    }

    inline void apply (juce::AudioProcessorValueTreeState& apvts, int preset)
    {
        if (! juce::isPositiveAndBelow (preset, names.size()))
            return;

        forEachParameter (apvts, [preset] (juce::RangedAudioParameter& p)
                          { set (p, valueFor (preset, p)); });
    }

    /// Whether every parameter still holds the preset's value.
    inline bool matches (juce::AudioProcessorValueTreeState& apvts, int preset)
    {
        auto same = juce::isPositiveAndBelow (preset, names.size());

        if (same)
            forEachParameter (apvts, [preset, &same] (juce::RangedAudioParameter& p)
                              { same = same && std::abs (p.getValue() - valueFor (preset, p)) < 1.0e-4f; });

        return same;
    }

    /// Every preset parameter to a new value, snapped to its steps where it has them.
    inline void randomise (juce::AudioProcessorValueTreeState& apvts)
    {
        std::mt19937 rng { std::random_device{}() };
        std::uniform_real_distribution<float> unit;

        forEachParameter (apvts, [&] (juce::RangedAudioParameter& p)
        {
            auto value = unit (rng);
            const auto steps = p.getNumSteps();

            if (steps > 1 && steps < 100000)
                value = std::round (value * static_cast<float> (steps - 1)) / static_cast<float> (steps - 1);

            set (p, value);
        });
    }
}
