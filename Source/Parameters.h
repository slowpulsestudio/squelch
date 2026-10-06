#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/** The parameter list, declared once.

    Everything that needs to know about parameters reads it from here: the
    APVTS layout, the preset system and Randomise. Keeping separate lists in
    separate places is how they end up disagreeing.

    Names follow prototype/params.py, which is current for parameter names and
    ranges. README.md is the design brief and still lists some of the names
    these replaced; it governs what each reaction has to do, not what the
    controls are called.
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
                                               "1/16T", "1/32", "1/32D", "1/32T",
                                               "1/64" };

    /// Grid division lengths in beats, in the same order as gridNames.
    inline constexpr float gridBeats[] { 4.0f, 2.0f, 1.0f, 1.5f, 2.0f / 3.0f,
                                         0.5f, 0.75f, 1.0f / 3.0f, 0.25f, 0.375f,
                                         1.0f / 6.0f, 0.125f, 0.1875f, 1.0f / 12.0f,
                                         0.0625f };

    /// ENRICHMENT spans this many dB either side of unity.
    inline constexpr float enrichmentRangeDb = 18.0f;

    struct Continuous
    {
        const char* id;
        const char* label;
        float defaultValue;
        const char* tooltip;

        /// What the knob says when the full name will not fit; the host sees the full name.
        const char* shortLabel = nullptr;
    };

    /** The knobs, in the order the prototype lists them.

        Tooltips open with the parameter's glyph from README.md, so the panel,
        the spec and the maths all name the same thing. Bracketed glyphs are
        labels rather than terms in the equations.
    */
    inline constexpr Continuous continuous[]
    {
        { ids::enrichment, "Enrichment", 0.5f,
          u8"\u03B7 \u2014 how hard the source is driven into the filter's feedback loop. "
          "The saturation sits inside that loop, so level is part of the character: "
          "this changes the voice, not the volume. \u03B7 is the enrichment factor "
          "in reactor physics." },
        { ids::flux, "Flux", 0.0f,
          u8"\u03C6 \u2014 timing jitter. Pulls each event off the grid by a random amount. \u03C6 is flux." },
        { ids::probability, "Probability", 0.65f,
          u8"\u03C1 \u2014 the chance a step fires at all. At full every step fires, which "
          "leaves no gaps for the source to breathe through. \u03C1 is probability density." },
        { ids::reactivity, "Reactivity", 0.3f,
          u8"\u03B1 \u2014 how much an accented step differs from an ordinary one. Accents "
          "open the sweep further and push the resonance closer to oscillation. \u03B1 is reactivity in reactor physics." },
        { ids::volatility, "Volatility", 0.3f,
          u8"\u03C3 \u2014 how far events scatter in time and stereo from where they should be. \u03C3 is standard deviation, which is what scatter is.",
          "Vty" },
        { ids::halfLife, "Half-Life", 0.0f,
          u8"\u03BB \u2014 how far one event's filter state carries into the next. \u03BB is the decay constant: half-life is ln2 over \u03BB.",
          "Half" },
        { ids::decay, "Decay", 0.3f,
          u8"\u03C4 \u2014 how long the filter takes to close again after an event opens it. "
          "Short is a blip, long is a wail. \u03C4 is the time constant of an exponential decay." },
        { ids::spread, "Spread", 0.75f,
          u8"\u0394f \u2014 how far the cutoff sweeps, in octaves. This is the size of the "
          "squelch: at the bottom the filter barely moves. \u0394f is an interval in frequency." },
        { ids::toxicity, "Toxicity", 0.45f,
          u8"\u03B4 \u2014 saturation inside the feedback loop. It grows with the resonance, "
          "so the filter limits its own ring and stays round rather than shrill. \u03B4 is the coefficient inside the tanh." },
        { ids::containment, "Containment", 0.0f,
          u8"\u03BA \u2014 closes the vessel: thins the events out until almost nothing escapes. \u03BA is an absorption coefficient.",
          "Ctn" },
        { ids::drive, "Drive", 0.3f,
          "Saturation after the filter. The level it adds is taken back out, so this "
          "is character rather than gain. No glyph: it sits outside the filter equation." },
        { ids::contamination, "Contamination", 0.25f,
          u8"\u03BD \u2014 the noise bed the reaction gives off. Its texture comes from the Reaction. \u03BD denotes noise.",
          "Ctm" },
        { ids::exposure, "Exposure", 0.6f,
          u8"k \u2014 feedback around the filter. At the top it approaches self-oscillation "
          "and the filter sings on its own, which is what lets it squelch on material "
          "that has nothing at the cutoff. k is the feedback gain of a ladder filter, and 4 is where it oscillates." },
        { ids::collimator, "Collimator", 0.0f,
          u8"(\u03B8) \u2014 narrows the beam: focuses the output toward the centre. \u03B8 is an angle, and a collimator narrows a beam." },
        { ids::fallout, "Fallout", 0.3f,
          u8"(\u03C7) \u2014 dispersal. Smears the reaction in pitch and stereo as it settles. \u03C7 denotes scattering." },
        { ids::afterglow, "Afterglow", 0.0f,
          u8"(T\u2086\u2080) \u2014 how long the reaction keeps glowing after it has happened. T60 is the acoustic term for decay time." },
        { ids::ionizeAmount, "Ionization Degree", 0.7f,
          u8"(\u03B9) \u2014 Charge: the degree of ionization, or how far Ionize scatters each event "
          "in stereo, spectrum and depth. \u03B9 for ionise.",
          "Charge" },
    };

    /// Tooltips for the controls that are choices rather than quantities.
    inline constexpr auto reactionTooltip =
        u8"(\u211B) \u2014 which character the filter takes: where it rests, how far it "
        "sweeps, how close to oscillation it runs, and which of its poles you hear. Script letters mark a choice rather than a quantity.";
    inline constexpr auto modeTooltip =
        u8"(\u2133) \u2014 what decides when events fire: the grid, chance, free running, "
        "or the incoming audio itself. Script letters mark a choice rather than a quantity.";
    inline constexpr auto gridTooltip =
        u8"(\u0393) \u2014 event rate, from a bar down to a 64th. \u0393 for the grid lattice.";
    inline constexpr auto seedTooltip =
        u8"(s\u2080) \u2014 which pattern the hash produces. Same seed, same pattern, every time. s0 is an initial state.";
    inline constexpr auto ionizeTooltip =
        u8"(\u03B9) \u2014 latched: scatters every event in stereo, spectrum and depth at once. \u03B9 for ionise.";
    inline constexpr auto meltdownTooltip =
        u8"(\u03A9) \u2014 momentary: a staged runaway. Containment, drive, toxicity, exposure "
        "and contamination arrive in sequence, then settle. \u03A9 is the end state.";
    inline constexpr auto clipTooltip =
        u8"(\u2308\u2309) \u2014 a hard ceiling instead of the lookahead limiter. Latency is "
        "padded to match, so switching never makes the host re-sync. The brackets are the ceiling function.";

    /** Parameters Randomise and the presets leave alone.

        SEED is deliberately not here: the studio rules call it creative
        variation rather than a global toggle, so it is randomised like the
        rest. CLIP and the Input/Output strips are the exclusions, because they
        are gain staging and session settings rather than sound design.

        MELTDOWN is momentary and IONIZE latched: both are performed, not recalled
        by a preset (a session may still save IONIZE engaged). IONIZEAMOUNT is
        part of the performance and stays out with them.
    */
    inline bool isExcludedFromPresets (const juce::String& id)
    {
        return id == ids::clip || id == ids::meltdown || id == ids::ionize
            || id == ids::ionizeAmount;
    }
}
