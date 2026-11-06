#include "procaudio/profile.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <map>
#include <stdexcept>

#include "procaudio/core.h"
#include "procaudio/dsp.h"

namespace pa {

using cvec = std::vector<std::complex<double>>;

const SurfaceSpec& surface_spec(const std::string& name) {
    static const std::map<std::string, SurfaceSpec> table = [] {
        std::map<std::string, SurfaceSpec> m;
        auto mk = [&](const char* n, double h, double g, double kr, double q,
                      double share, const char* note) -> SurfaceSpec& {
            SurfaceSpec s;
            s.name = n; s.note = note; s.h_rms = h; s.gamma = g; s.k_r = kr;
            s.q = q; s.reson_share = share;
            return m[n] = s;
        };
        auto feat = [](SurfaceSpec& s, double period, double width, double height) {
            s.feat_period = period; s.feat_width = width; s.feat_height = height;
        };
        auto grit = [](SurfaceSpec& s, double per_m) {
            s.grit_per_m = per_m; s.grit_height = 15e-6; s.grit_sigma = 0.8;
            s.grit_width = 0.4e-3;
        };
        auto& wood = mk("wood-floor", 6e-6, 2.6, 600.0, 3.0, 0.5,
                        "sanded boards, grain ~1.7 mm, a groove between 12 cm "
                        "boards, a little household grit");
        feat(wood, 0.12, 1.5e-3, -3e-4); grit(wood, 15.0);
        auto& varn = mk("varnished-wood-floor", 1.5e-6, 2.6, 600.0, 3.0, 0.1,
                        "varnished boards (grain filled, 1.5 um rms), a groove "
                        "between 12 cm boards, a little household grit");
        feat(varn, 0.12, 1.5e-3, -3e-4); grit(varn, 15.0);
        auto& tight = mk("varnished-wood-floor-tight", 1.5e-6, 2.6, 600.0, 3.0, 0.1,
                         "varnished boards (1.5 um rms), tight flush joints "
                         "every 12 cm (1 mm, 0.1 mm deep), a little grit");
        feat(tight, 0.12, 1.0e-3, -1e-4); grit(tight, 15.0);
        auto& tile = mk("tile-floor", 0.5e-6, 2.8, 2000.0, 2.0, 0.1,
                        "glazed ceramic tiles, 30 cm grid, 4 mm grout lines "
                        "1.2 mm deep, a little grit");
        feat(tile, 0.30, 4e-3, -1.2e-3); grit(tile, 80.0);
        auto& rug = mk("rug", 150e-6, 2.0, 330.0, 1.0, 0.4,
                       "cut-pile rug: tufts ~3 mm, very soft and lossy");
        rug.l_corr = 3e-3;
        mk("concrete-slab", 60e-6, 2.4, 250.0, 1.5, 0.3,
           "broom-finish concrete: rough, aggregate texture ~4 mm");
        auto& steel = mk("steel-plate-floor", 1.5e-6, 2.8, 1500.0, 4.0, 0.3,
                         "smooth steel with a raised tread pattern every 25 mm");
        feat(steel, 0.025, 5e-3, 4e-4);
        return m;
    }();
    return table.at(name);
}

// real signal from a one-sided amplitude spectrum with random phases
static std::vector<double> random_phase_signal(const std::vector<double>& amp,
                                               std::size_t n, Rng& rng) {
    cvec X(n, {0.0, 0.0});
    for (std::size_t i = 0; i <= n / 2; ++i) {
        double ph = 2.0 * kPi * rng.uniform();
        std::complex<double> v = amp[i] * std::polar(1.0, ph);
        if (i == 0 || i == n / 2) v = {v.real(), 0.0};
        X[i] = v;
        if (i > 0 && i < n / 2) X[n - i] = std::conj(v);
    }
    fft(X, true);
    std::vector<double> z(n);
    double ss = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        z[i] = X[i].real();
        ss += z[i] * z[i];
    }
    double rms = std::sqrt(ss / static_cast<double>(n));
    if (rms > 0.0)
        for (auto& v : z) v /= rms;
    return z;
}

static void spectral_gain(std::vector<double>& z, double dx,
                          double (*gain)(double, double), double param) {
    const std::size_t n = z.size();
    cvec X(z.begin(), z.end());
    fft(X, false);
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t kk = i <= n / 2 ? i : n - i;
        double k = static_cast<double>(kk) / (static_cast<double>(n) * dx);
        X[i] *= gain(k, param);
    }
    fft(X, true);
    for (std::size_t i = 0; i < n; ++i) z[i] = X[i].real();
}

static double gauss_gain(double k, double sigma) {
    double a = 2.0 * kPi * k * sigma;
    return std::exp(-0.5 * a * a);
}

void gaussian_smooth(std::vector<double>& z, double sigma, double dx) {
    spectral_gain(z, dx, gauss_gain, sigma);
}

std::vector<double> surface_tile(const SurfaceSpec& spec, std::size_t n,
                                 double sigma_c, uint64_t seed) {
    if (!is_pow2(n)) throw std::invalid_argument("tile size must be 2^k");
    Rng rng(seed);
    const double L = static_cast<double>(n) * kDX;
    std::vector<double> af(n / 2 + 1), ar(n / 2 + 1);
    for (std::size_t i = 0; i <= n / 2; ++i) {
        double k = static_cast<double>(i) / L;
        bool band = k > 10.0;
        af[i] = band ? std::pow(std::max(k, 1.0), -spec.gamma / 2.0) : 0.0;
        double bw = spec.k_r / spec.q;
        ar[i] = band ? std::exp(-0.5 * std::pow((k - spec.k_r) / bw, 2.0)) : 0.0;
    }
    // Python draws ONE phase vector for both parts; do the same
    Rng phase_rng = rng;
    std::vector<double> fractal = random_phase_signal(af, n, phase_rng);
    Rng phase_rng2 = rng;
    std::vector<double> reson = random_phase_signal(ar, n, phase_rng2);
    rng = phase_rng;
    const double s = spec.reson_share;
    std::vector<double> z(n);
    for (std::size_t i = 0; i < n; ++i)
        z[i] = spec.h_rms * (std::sqrt(1.0 - s) * fractal[i] + std::sqrt(s) * reson[i]);

    if (spec.feat_period > 0.0) {
        // groove/bump centres at period/2 + j period, laid around the tile
        double sig = spec.feat_width / 2.355;
        long half = static_cast<long>(std::ceil(5.0 * sig / kDX));
        for (double c = spec.feat_period / 2.0; c < L; c += spec.feat_period) {
            long ci = static_cast<long>(std::llround(c / kDX));
            for (long d = -half; d <= half; ++d) {
                long idx = ((ci + d) % static_cast<long>(n) + static_cast<long>(n)) %
                           static_cast<long>(n);
                double dd = (static_cast<double>(ci + d) * kDX - c) / sig;
                z[static_cast<std::size_t>(idx)] += spec.feat_height * std::exp(-0.5 * dd * dd);
            }
        }
    }
    if (spec.grit_per_m > 0.0) {
        std::vector<double> g(n, 0.0);
        double s_pts = spec.grit_width / 2.355 / kDX;
        long half = static_cast<long>(4.0 * s_pts) + 1;
        double pos = rng.exponential(1.0 / spec.grit_per_m);
        while (pos < L) {
            double h = spec.grit_height * std::exp(spec.grit_sigma * rng.normal());
            long p = static_cast<long>(pos / kDX);
            for (long d = -half; d <= half; ++d) {
                long idx = ((p + d) % static_cast<long>(n) + static_cast<long>(n)) %
                           static_cast<long>(n);
                double b = h * std::exp(-0.5 * std::pow(d / s_pts, 2.0));
                auto& gi = g[static_cast<std::size_t>(idx)];
                gi = std::max(gi, b);
            }
            pos += rng.exponential(1.0 / spec.grit_per_m);
        }
        for (std::size_t i = 0; i < n; ++i) z[i] += g[i];
    }
    gaussian_smooth(z, sigma_c, kDX);
    return z;
}

std::vector<double> dilate_by_circle(const std::vector<double>& z, double radius,
                                     double dx) {
    const std::size_t n = z.size();
    auto mm = std::minmax_element(z.begin(), z.end());
    double span = *mm.second - *mm.first;
    double w = std::min(std::sqrt(2.0 * radius * std::max(span, dx * 1e-3)),
                        0.5 * radius);
    long half = std::max(1L, static_cast<long>(w / dx));
    std::vector<double> sag(2 * half + 1);
    for (long u = -half; u <= half; ++u) {
        double x = u * dx;
        sag[u + half] = radius - std::sqrt(std::max(radius * radius - x * x, 0.0));
    }
    std::vector<double> s(n);
    const long N = static_cast<long>(n);
    for (long i = 0; i < N; ++i) {
        double best = -1e300;
        for (long u = -half; u <= half; ++u) {
            long j = i + u;
            j = (j % N + N) % N;
            double v = z[static_cast<std::size_t>(j)] - sag[u + half];
            if (v > best) best = v;
        }
        s[static_cast<std::size_t>(i)] = best;
    }
    return s;
}

std::vector<double> rim_profile(const RimSpec& spec, std::size_t n, double radius,
                                uint64_t seed) {
    std::vector<double> z(n, 0.0);
    const double circ = 2.0 * kPi * radius;
    for (std::size_t i = 0; i < n; ++i) {
        double s = static_cast<double>(i) / static_cast<double>(n);
        z[i] += spec.ecc * std::cos(2.0 * kPi * s);
        for (const auto& b : spec.bumps) {
            double d = std::fmod(s - b.pos + 0.5, 1.0);
            if (d < 0.0) d += 1.0;
            d -= 0.5;
            double sig = b.width / circ / 2.355;
            z[i] += b.height * std::exp(-0.5 * (d / sig) * (d / sig));
        }
    }
    if (spec.rough_h_rms > 0.0) {
        Rng rng(seed + 11);
        double dx = circ / static_cast<double>(n);
        std::vector<double> amp(n / 2 + 1);
        for (std::size_t i = 0; i <= n / 2; ++i) {
            double k = static_cast<double>(i) / (static_cast<double>(n) * dx);
            amp[i] = i > 0 ? std::pow(std::max(k, 1.0), -spec.rough_gamma / 2.0) : 0.0;
        }
        std::vector<double> zr = random_phase_signal(amp, n, rng);
        for (std::size_t i = 0; i < n; ++i) z[i] += spec.rough_h_rms * zr[i];
    }
    if (spec.lumps > 0.0) {
        Rng rng(seed + 7);
        std::vector<double> zl(n, 0.0);
        for (int o = 2; o <= 8; ++o) {
            double a = rng.normal() / o, ph = rng.uniform() * 2.0 * kPi;
            for (std::size_t i = 0; i < n; ++i)
                zl[i] += a * std::cos(2.0 * kPi * o * static_cast<double>(i) /
                                      static_cast<double>(n) + ph);
        }
        double ss = 0.0;
        for (double v : zl) ss += v * v;
        double sd = std::sqrt(ss / static_cast<double>(n));
        if (sd > 0.0)
            for (std::size_t i = 0; i < n; ++i) z[i] += spec.lumps * zl[i] / sd;
    }
    return z;
}

}  // namespace pa
