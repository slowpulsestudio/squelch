#pragma once

#include <cstdint>

/** Deterministic hashing for every probabilistic choice.

    Nothing draws from a running generator. Each decision hashes its own
    position with the seed, so a bounce matches what was heard and replaying a
    bar fires the same pattern. The constants and shift amounts have to match
    prototype/rng.py exactly or the C++ schedules a different piece of music.
*/
namespace squelch::rng
{
    inline std::uint64_t uhash (std::initializer_list<std::uint64_t> values) noexcept
    {
        std::uint64_t h = 0xCBF29CE484222325ULL;

        for (auto v : values)
        {
            h ^= v;
            h *= 0x100000001B3ULL;
            h ^= h >> 33;
            h *= 0xFF51AFD7ED558CCDULL;
            h ^= h >> 29;
        }

        return h;
    }

    /// Uniform in [0, 1).
    inline double urand (std::initializer_list<std::uint64_t> values) noexcept
    {
        return static_cast<double> (uhash (values) >> 11) / 9007199254740992.0;
    }

    inline double urandRange (double lo, double hi,
                              std::initializer_list<std::uint64_t> values) noexcept
    {
        return lo + (hi - lo) * urand (values);
    }

    /// Uniform in [-1, 1).
    inline double ubipolar (std::initializer_list<std::uint64_t> values) noexcept
    {
        return urand (values) * 2.0 - 1.0;
    }
}
