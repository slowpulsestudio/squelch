/** Measurement tools for the validation suite.

    dsp-testing.md's Tests 6 to 11 are all of the form "prove the mechanism is
    the one specified, not a cheaper thing that sounds similar". That needs
    spectra, harmonic amplitudes, autocorrelation and stereo correlation, so
    they live here rather than cluttering the tests themselves.

    Everything is double precision and allocates freely: this is offline
    measurement, not the audio thread.
*/

#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace squelch::analysis
{
    using Complex = std::complex<double>;

    /** In-place radix-2 FFT. `v` must be a power of two in length. */
    inline void fft (std::vector<Complex>& v)
    {
        const auto n = v.size();
        if (n <= 1)
            return;

        for (std::size_t i = 1, j = 0; i < n; ++i)
        {
            auto bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
                std::swap (v[i], v[j]);
        }

        for (std::size_t len = 2; len <= n; len <<= 1)
        {
            const auto angle = -2.0 * M_PI / double (len);
            const Complex step { std::cos (angle), std::sin (angle) };

            for (std::size_t i = 0; i < n; i += len)
            {
                Complex w { 1.0, 0.0 };
                for (std::size_t k = 0; k < len / 2; ++k)
                {
                    const auto u = v[i + k];
                    const auto t = v[i + k + len / 2] * w;
                    v[i + k] = u + t;
                    v[i + k + len / 2] = u - t;
                    w *= step;
                }
            }
        }
    }

    inline std::size_t nextPowerOfTwo (std::size_t n)
    {
        std::size_t p = 1;
        while (p < n)
            p <<= 1;
        return p;
    }

    /** Magnitude spectrum of a Hann-windowed segment, bins 0..N/2. */
    inline std::vector<double> spectrum (const std::vector<double>& x,
                                         std::size_t from = 0,
                                         std::size_t length = 0)
    {
        if (length == 0 || from + length > x.size())
            length = x.size() > from ? x.size() - from : 0;
        if (length < 4)
            return {};

        const auto n = nextPowerOfTwo (length);
        std::vector<Complex> buffer (n, Complex { 0.0, 0.0 });

        for (std::size_t i = 0; i < length; ++i)
        {
            const auto w = 0.5 - 0.5 * std::cos (2.0 * M_PI * double (i) / double (length - 1));
            buffer[i] = Complex { x[from + i] * w, 0.0 };
        }

        fft (buffer);

        std::vector<double> mag (n / 2 + 1);
        for (std::size_t i = 0; i < mag.size(); ++i)
            mag[i] = std::abs (buffer[i]);
        return mag;
    }

    /** Energy at a single frequency, by Goertzel. Immune to bin alignment in
        a way that picking the nearest FFT bin is not, which matters when the
        test is "is there energy at exactly f_h/2".
    */
    inline double goertzel (const std::vector<double>& x, double hz, double sr,
                            std::size_t from = 0, std::size_t length = 0)
    {
        if (length == 0 || from + length > x.size())
            length = x.size() > from ? x.size() - from : 0;
        if (length < 4 || hz <= 0.0 || hz >= sr * 0.5)
            return 0.0;

        const auto w = 2.0 * M_PI * hz / sr;
        const auto coefficient = 2.0 * std::cos (w);
        double s1 = 0.0, s2 = 0.0;

        for (std::size_t i = 0; i < length; ++i)
        {
            const auto window = 0.5 - 0.5 * std::cos (2.0 * M_PI * double (i) / double (length - 1));
            const auto s = x[from + i] * window + coefficient * s1 - s2;
            s2 = s1;
            s1 = s;
        }

        const auto real = s1 - s2 * std::cos (w);
        const auto imaginary = s2 * std::sin (w);
        return 2.0 * std::sqrt (real * real + imaginary * imaginary) / double (length);
    }

    /** Total harmonic distortion of a sine response: harmonics 2..n over the
        fundamental. Returns 0 when there is no fundamental to divide by.
    */
    inline double thd (const std::vector<double>& x, double fundamental, double sr, int harmonics = 8)
    {
        const auto a1 = goertzel (x, fundamental, sr);
        if (a1 < 1e-12)
            return 0.0;

        auto sum = 0.0;
        for (int k = 2; k <= harmonics; ++k)
        {
            const auto f = fundamental * k;
            if (f >= sr * 0.45)
                break;
            const auto a = goertzel (x, f, sr);
            sum += a * a;
        }
        return std::sqrt (sum) / a1;
    }

    /** Energy in the even harmonics and in the odd ones, separately. Test 9's
        asymmetric saturation is specified by the even orders it creates.
    */
    inline void harmonicSplit (const std::vector<double>& x, double fundamental, double sr,
                               double& even, double& odd, int harmonics = 9)
    {
        even = 0.0;
        odd = 0.0;
        for (int k = 2; k <= harmonics; ++k)
        {
            const auto f = fundamental * k;
            if (f >= sr * 0.45)
                break;
            const auto a = goertzel (x, f, sr);
            (k % 2 == 0 ? even : odd) += a * a;
        }
        even = std::sqrt (even);
        odd = std::sqrt (odd);
    }

    /** Spectral centroid in Hz of one windowed segment. */
    inline double centroid (const std::vector<double>& x, double sr,
                            std::size_t from = 0, std::size_t length = 0)
    {
        const auto mag = spectrum (x, from, length);
        if (mag.empty())
            return 0.0;

        const auto binHz = sr / double ((mag.size() - 1) * 2);
        double weighted = 0.0, total = 0.0;

        for (std::size_t i = 1; i < mag.size(); ++i)
        {
            weighted += double (i) * binHz * mag[i];
            total += mag[i];
        }
        return total > 1e-15 ? weighted / total : 0.0;
    }

    /** Variance of the spectral centroid across successive frames: how much
        the spectrum is moving, rather than where it sits.
    */
    inline double centroidVariance (const std::vector<double>& x, double sr, std::size_t frame = 2048)
    {
        std::vector<double> values;
        for (std::size_t at = 0; at + frame <= x.size(); at += frame / 2)
            values.push_back (centroid (x, sr, at, frame));

        if (values.size() < 2)
            return 0.0;

        auto mean = 0.0;
        for (auto v : values)
            mean += v;
        mean /= double (values.size());

        auto variance = 0.0;
        for (auto v : values)
            variance += (v - mean) * (v - mean);
        return variance / double (values.size());
    }

    /** Normalised autocorrelation at lag k of a mean-removed signal. */
    inline double autocorrelation (const std::vector<double>& x, std::size_t lag)
    {
        if (x.size() <= lag + 1)
            return 0.0;

        auto mean = 0.0;
        for (auto v : x)
            mean += v;
        mean /= double (x.size());

        double numerator = 0.0, denominator = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i)
        {
            const auto d = x[i] - mean;
            denominator += d * d;
            if (i + lag < x.size())
                numerator += d * (x[i + lag] - mean);
        }
        return denominator > 1e-18 ? numerator / denominator : 0.0;
    }

    /** Pearson correlation between two channels: +1 mono, 0 uncorrelated,
        -1 antiphase.
    */
    inline double correlation (const std::vector<double>& a, const std::vector<double>& b)
    {
        const auto n = std::min (a.size(), b.size());
        if (n < 2)
            return 0.0;

        double ma = 0.0, mb = 0.0;
        for (std::size_t i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
        ma /= double (n);
        mb /= double (n);

        double num = 0.0, da = 0.0, db = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const auto x = a[i] - ma, y = b[i] - mb;
            num += x * y;
            da += x * x;
            db += y * y;
        }
        const auto d = std::sqrt (da * db);
        return d > 1e-18 ? num / d : 0.0;
    }

    /** Dominant frequency by parabolic interpolation around the strongest
        bin. More accurate than the bin centre, which at 2048 points is 21 Hz
        wide and would not resolve "is this f_h or f_h/2" at low frequencies.
    */
    inline double dominantHz (const std::vector<double>& x, double sr,
                              std::size_t from = 0, std::size_t length = 0)
    {
        const auto mag = spectrum (x, from, length);
        if (mag.size() < 4)
            return 0.0;

        std::size_t peak = 1;
        for (std::size_t i = 2; i + 1 < mag.size(); ++i)
            if (mag[i] > mag[peak])
                peak = i;

        const auto binHz = sr / double ((mag.size() - 1) * 2);
        const auto a = mag[peak - 1], b = mag[peak], c = mag[peak + 1];
        const auto denominator = a - 2.0 * b + c;
        const auto shift = std::abs (denominator) > 1e-18 ? 0.5 * (a - c) / denominator : 0.0;
        return (double (peak) + shift) * binHz;
    }

    /** How deep the deepest notch is, in dB, over a frequency range. Test 8
        asks for spectral notches and their movement.
    */
    inline double deepestNotchDb (const std::vector<double>& x, double sr, double loHz, double hiHz,
                                  std::size_t from = 0, std::size_t length = 0)
    {
        const auto mag = spectrum (x, from, length);
        if (mag.size() < 8)
            return 0.0;

        const auto binHz = sr / double ((mag.size() - 1) * 2);
        const auto lo = std::max<std::size_t> (1, std::size_t (loHz / binHz));
        const auto hi = std::min (mag.size() - 1, std::size_t (hiHz / binHz));
        if (hi <= lo + 2)
            return 0.0;

        double peak = 0.0, trough = 1e30;
        for (auto i = lo; i <= hi; ++i)
        {
            peak = std::max (peak, mag[i]);
            trough = std::min (trough, mag[i]);
        }
        if (peak < 1e-15)
            return 0.0;
        return 20.0 * std::log10 (std::max (trough, 1e-15) / peak);
    }

    /** The peak of the out-over-reference magnitude ratio, and where it is.

        What counts as a resonance depends on what you divide by. Dividing by
        the input leaves the whole filter response, which for a lowpass is
        monotonic and peaks at DC whether it resonates or not. Dividing by
        the same filter with its feedback off cancels the slope and leaves
        only what the feedback added, which is the thing being asked about.
    */
    inline void ratioPeak (const std::vector<double>& reference, const std::vector<double>& out,
                           double sr, double loHz, double hiHz,
                           double& peakDb, double& peakHz,
                           std::size_t from = 0, std::size_t length = 16384)
    {
        peakDb = -1e30;
        peakHz = 0.0;

        const auto a = spectrum (reference, from, length);
        const auto b = spectrum (out, from, length);
        if (a.size() < 8 || a.size() != b.size())
            return;

        const auto binHz = sr / double ((a.size() - 1) * 2);
        const auto lo = std::max<std::size_t> (1, std::size_t (loHz / binHz));
        const auto hi = std::min (a.size() - 1, std::size_t (hiHz / binHz));

        for (auto i = lo; i <= hi; ++i)
        {
            const auto value = 20.0 * std::log10 (std::max (b[i], 1e-18) / std::max (a[i], 1e-18));
            if (value > peakDb)
            {
                peakDb = value;
                peakHz = double (i) * binHz;
            }
        }

        if (peakDb < -1e29)
            peakDb = 0.0;
    }

    /** How far the out-over-input peak wanders frame to frame, relative to
        its mean. Dividing by the input first means this measures the
        resonator moving rather than the excitation's own spectral jitter.
    */
    inline double peakMovement (const std::vector<double>& in, const std::vector<double>& out,
                                double sr, double loHz, double hiHz, std::size_t frame = 8192)
    {
        std::vector<double> values;
        for (std::size_t at = 0; at + frame <= out.size(); at += frame / 2)
        {
            double peakDb = 0.0, peakHz = 0.0;
            ratioPeak (in, out, sr, loHz, hiHz, peakDb, peakHz, at, frame);
            if (peakHz > 0.0)
                values.push_back (peakHz);
        }
        if (values.size() < 2)
            return 0.0;

        auto mean = 0.0;
        for (auto v : values)
            mean += v;
        mean /= double (values.size());

        auto variance = 0.0;
        for (auto v : values)
            variance += (v - mean) * (v - mean);
        return std::sqrt (variance / double (values.size())) / std::max (mean, 1e-9);
    }

    /** The log-magnitude transfer curve of out over reference, in dB, across
        a band. The shape of the comb, rather than one number about it.
    */
    inline std::vector<double> transferCurve (const std::vector<double>& reference,
                                              const std::vector<double>& out,
                                              double sr, double loHz, double hiHz,
                                              std::size_t from, std::size_t length)
    {
        const auto a = spectrum (reference, from, length);
        const auto b = spectrum (out, from, length);
        if (a.size() < 8 || a.size() != b.size())
            return {};

        const auto binHz = sr / double ((a.size() - 1) * 2);
        const auto lo = std::max<std::size_t> (1, std::size_t (loHz / binHz));
        const auto hi = std::min (a.size() - 1, std::size_t (hiHz / binHz));

        std::vector<double> curve;
        curve.reserve (hi - lo + 1);
        for (auto i = lo; i <= hi; ++i)
            curve.push_back (20.0 * std::log10 (std::max (b[i], 1e-18) / std::max (a[i], 1e-18)));
        return curve;
    }

    /** How uneven that curve is once its overall shape is removed, in dB.

        A comb ripples bin to bin; a resonator has a steep but smooth slope.
        Taking the standard deviation of the raw curve cannot tell them
        apart -- a 320 Hz resonator measured across 100 Hz to 4 kHz reads 16
        dB of "ripple" with no comb present at all. Subtracting a moving
        average over the curve leaves only the fast variation, which is the
        interference.

        Also more robust than hunting for the single deepest bin, which on a
        noise excitation finds whichever bin happened to come out quietest.
    */
    inline double ripple (const std::vector<double>& curve, int span = 31)
    {
        if (curve.size() < std::size_t (span) * 2)
            return 0.0;

        const auto half = std::size_t (span / 2);
        auto variance = 0.0;
        std::size_t count = 0;

        for (std::size_t i = half; i + half < curve.size(); ++i)
        {
            auto local = 0.0;
            for (std::size_t j = i - half; j <= i + half; ++j)
                local += curve[j];
            local /= double (span);

            const auto d = curve[i] - local;
            variance += d * d;
            ++count;
        }
        return count > 0 ? std::sqrt (variance / double (count)) : 0.0;
    }

    inline double db (double a, double b)
    {
        return 20.0 * std::log10 (std::max (a, 1e-15) / std::max (b, 1e-15));
    }
}
