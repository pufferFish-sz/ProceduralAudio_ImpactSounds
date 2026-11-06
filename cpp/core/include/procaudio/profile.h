// Surface and rim height profiles (proto/core/slide_profile.py
// surface_profile / grit_profile, proto/core/rolling.py dilate_by_circle /
// gaussian_smooth / object_profile).
//
// Real-time difference: Python builds one non-periodic table as long as the
// render; here every table is a PERIODIC tile (2^k points on the kDX grid,
// 2.6 m at 2^17) built once off the audio thread and read at x mod length.
// Spectral parts are periodic by construction; board joints and grit are
// laid out with wrap-around.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pa {

struct SurfaceSpec {
    std::string name, note;
    double h_rms = 0.0;        // m, random roughness rms
    double gamma = 2.6;        // PSD slope k^-gamma
    double k_r = 600.0;        // 1/m, texture resonance (heard at k_r v)
    double q = 3.0;
    double reson_share = 0.0;  // power share of the resonance
    double feat_period = 0.0;  // m; 0 = no meso features (board joints, grout)
    double feat_width = 0.0;   // m, FWHM
    double feat_height = 0.0;  // m, negative = groove
    double grit_per_m = 0.0;   // loose particles per metre; 0 = none
    double grit_height = 0.0;  // m, lognormal median
    double grit_sigma = 0.8;
    double grit_width = 0.4e-3;  // m, FWHM
    double l_corr = 1e-3;      // m, roughness correlation across the track
};

// SURFACE_PROFILES presets. Throws std::out_of_range for unknown names.
const SurfaceSpec& surface_spec(const std::string& name);

// Periodic tile of n (power of two) points on the kDX grid, averaged over a
// contact patch of Gaussian sigma sigma_c.
std::vector<double> surface_tile(const SurfaceSpec& spec, std::size_t n,
                                 double sigma_c, uint64_t seed);

// Upper envelope traced by a rigid circle of `radius` rolling over a
// PERIODIC profile z on grid dx.
std::vector<double> dilate_by_circle(const std::vector<double>& z,
                                     double radius, double dx);

// Periodic Gaussian average (sigma m) on grid dx; size must be 2^k.
void gaussian_smooth(std::vector<double>& z, double sigma, double dx);

// The rolling object's own rim profile, one circumference.
struct RimSpec {
    struct Bump { double pos, height, width; };   // turns, m (+ raised), m FWHM
    double ecc = 0.0;                 // m, once-per-revolution eccentricity
    std::vector<Bump> bumps;          // seams, dents, stitches
    double rough_h_rms = 0.0;         // m, surface texture ("combined roughness")
    double rough_gamma = 2.2;
    double lumps = 0.0;               // m, random orders 2-8
};

// n (power of two) points around the rim, spacing 2 pi radius / n.
std::vector<double> rim_profile(const RimSpec& spec, std::size_t n,
                                double radius, uint64_t seed);

}  // namespace pa
