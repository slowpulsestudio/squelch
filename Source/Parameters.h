#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/** The parameter list, declared once.

    Everything that needs to know about parameters reads it from here: the
    APVTS layout, the preset system and Randomise. Keeping separate lists in
    separate places is how they end up disagreeing.

    Names follow prototype/params.py, which is the live spec. prompt.md is the
    original brief and is kept as a record, so it still lists the names these
    replaced.
*/
namespace squelch
{
    namespace ids
    {
        inline constexpr auto reaction      = "REACTION";
        inline constexpr auto mode          = "MODE";
        inline constexpr auto grid          = "GRID";

        inline constexpr auto enrichment    = "ENRICHMENT";
        inline constexpr auto flux          = "FLUX";
        inline constexpr auto probability   = "PROBABILITY";
        inline constexpr auto reactivity    = "REACTIVITY";
        inline constexpr auto volatility    = "VOLATILITY";
        inline constexpr auto halfLife      = "HALFLIFE";
        inline constexpr auto decay         = "DECAY";
        inline constexpr auto spread        = "SPREAD";
        inline constexpr auto toxicity      = "TOXICITY";
        inline constexpr auto containment   = "CONTAINMENT";
        inline constexpr auto drive         = "DRIVE";
        inline constexpr auto contamination = "CONTAMINATION";
        inline constexpr auto exposure      = "EXPOSURE";
        inline constexpr auto collimator    = "COLLIMATOR";
        inline constexpr auto fallout       = "FALLOUT";
        inline constexpr auto afterglow     = "AFTERGLOW";

        inline constexpr auto ionize        = "IONIZE";
        inline constexpr auto ionizeAmount  = "IONIZEAMOUNT";
        inline constexpr auto meltdown      = "MELTDOWN";

        inline constexpr auto seed          = "SEED";
        inline constexpr auto clip          = "CLIP";
    }

    inline const juce::StringArray reactionNames { "RADIATION", "FISSION", "SLUDGE",
                                                   "CHEMICAL", "ALIEN" };

    inline const juce::StringArray modeNames { "GRID", "RANDOM", "FREE", "INPUT" };

    inline const juce::StringArray gridNames { "1/1", "1/2", "1/4", "1/4D", "1/4T",
                                               "1/8", "1/8D", "1/8T", "1/16", "1/16D",
                                               "1/16T", "1/32" };

    /// Grid division lengths in beats, in the same order as gridNames.
    inline constexpr float gridBeats[] { 4.0f, 2.0f, 1.0f, 1.5f, 2.0f / 3.0f,
                                         0.5f, 0.75f, 1.0f / 3.0f, 0.25f, 0.375f,
                                         1.0f / 6.0f, 0.125f };

    /// ENRICHMENT spans this many dB either side of unity.
    inline constexpr float enrichmentRangeDb = 18.0f;

    struct Continuous
    {
        const char* id;
        const char* label;
        float defaultValue;
        const char* tooltip;
    };

    /// The knobs, in the order the prototype lists them.
    inline constexpr Continuous continuous[]
    {
        { ids::enrichment, "Enrichment", 0.5f,
          "How hard the source is fed into the reactor. The saturation has a fixed "
          "threshold, so this decides how much of it is reached: it changes character, "
          "not level." },
        { ids::flux, "Flux", 0.0f,
          "Timing jitter. Pulls each event off the grid by a random amount." },
        { ids::probability, "Probability", 1.0f,
          "Whether a reaction happens at all on a given step." },
        { ids::reactivity, "Reactivity", 0.3f,
          "How much happens when a reaction does fire." },
        { ids::volatility, "Volatility", 0.3f,
          "How far events scatter in time and stereo from where they should be." },
        { ids::halfLife, "Half-Life", 0.0f,
          "How long one reaction keeps influencing the reactions after it." },
        { ids::decay, "Decay", 0.3f,
          "How long the current reaction stays audible." },
        { ids::spread, "Spread", 0.4f,
          "How far the reaction's components separate from each other." },
        { ids::toxicity, "Toxicity", 0.5f,
          "How thick and saturated the reaction becomes." },
        { ids::containment, "Containment", 0.0f,
          "Closes the vessel: thins the events out until almost nothing escapes." },
        { ids::drive, "Drive", 0.3f,
          "Saturation. The level it adds is taken back out, so this is character "
          "rather than gain." },
        { ids::contamination, "Contamination", 0.25f,
          "The noise bed the reaction gives off. Its texture comes from the Reaction." },
        { ids::exposure, "Exposure", 0.5f,
          "Resonance. How sharply the filter peaks as it sweeps." },
        { ids::collimator, "Collimator", 0.0f,
          "Narrows the beam: focuses the output toward the centre." },
        { ids::fallout, "Fallout", 0.3f,
          "Dispersal. Smears the reaction in pitch and stereo as it settles." },
        { ids::afterglow, "Afterglow", 0.0f,
          "How long the reaction keeps glowing after it has happened." },
        { ids::ionizeAmount, "Ionize Amount", 0.7f,
          "How far Ionize scatters each event in stereo, spectrum and depth." },
    };

    /** Parameters Randomise and the presets leave alone.

        SEED is deliberately not here: the studio rules call it creative
        variation rather than a global toggle, so it is randomised like the
        rest. CLIP and the Input/Output strips are the exclusions, because they
        are gain staging and session settings rather than sound design.

        MELTDOWN is momentary. Storing it would recall a plugin mid-gesture.
    */
    inline bool isExcludedFromPresets (const juce::String& id)
    {
        return id == ids::clip || id == ids::meltdown;
    }
}
