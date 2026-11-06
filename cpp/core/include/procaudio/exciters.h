// Contact exciters: they turn motion into a contact-force signal (N) that
// drives SoundingBody objects.
//   simulate_impact_implicit  proto impact._simulate_implicit
//   RollExciter               proto rolling.roll_force, streaming
//   SlideExciter              proto slide_profile.profile_slide_force
//                             (model C: tracks, oversampling), streaming
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "procaudio/core.h"
#include "procaudio/dsp.h"
#include "procaudio/profile.h"

namespace pa {

// ------------------------------------------------------------ impact

struct ImpactResult {
    std::vector<double> force;   // N, at fs, from first contact to separation
    double contact_time = 0.0;   // s, first contiguous contact
    double f_max = 0.0;
    double v_out = 0.0;          // striker velocity after (negative = rebound)
    int n_contacts = 0;
    bool stable = true;
};

// Striker (mass m_s, speed v_in) into a modal resonator, Hunt-Crossley
// force f = k x^alpha + lam x^alpha xdot (f >= 0), trapezoidal (bilinear)
// discretization, one scalar implicit solve per sample (bracketed Newton).
// `oversample` > 1 runs the solve at oversample * fs and block-averages the
// force back to fs (proto contact.force_at_rate: preserves the impulse).
ImpactResult simulate_impact_implicit(double m_s, double v_in, double k,
                                      double alpha, double lam,
                                      const std::vector<Mode>& modes, double fs,
                                      double t_contact_max = 0.05,
                                      int oversample = 1);

// ------------------------------------------------------------ rolling

struct RollParams {
    double radius = 0.02;     // m
    double mass = 0.1;        // kg
    double k = 1e8;           // contact law k (N/m^alpha)
    double alpha = 1.5;       // 1.5 sphere (Hertz point), 1.0 cylinder (line)
    double zeta = 0.22;       // contact damping ratio (micro-landing restitution)
    double track_width = 0.0; // m, line-contact length (0 = point contact)
    double t_on = 3e-4;       // s, damping build-up after touchdown
    int oversample = 0;       // 0 = auto (4..16 from the contact resonance)
    uint64_t seed = 0;
    std::size_t tile_points = std::size_t(1) << 17;   // 2.6 m surface tile
};

// One vertical degree of freedom riding the circle-dilated, contact-averaged
// surface tile plus the object's rim profile; Kelvin-Voigt contact with the
// touchdown ramp. process(): one audio-rate speed sample (m/s, >= 0) in, one
// normal-force fluctuation sample (N, f - m g) out. Output lags the Python
// batch version by Decimator::kDelay samples.
using SplinePtr = std::shared_ptr<const PeriodicSpline>;

// Read-only tables of one rolling object on one surface. They depend on the
// object (radius, Hertz contact width, rim) and the surface, not on fs or on
// the instance, so a cache can build them once and share them.
struct RollTables {
    SplinePtr surface, rim;
};

class RollExciter {
public:
    // build the tables (off the audio thread: FFTs + circle dilation)
    static RollTables build_tables(const RollParams& p, const SurfaceSpec& surface,
                                   const RimSpec& rim);
    void prepare(double fs, const RollParams& p, const SurfaceSpec& surface,
                 const RimSpec& rim);
    // shared tables from build_tables (cheap: no table work)
    void prepare_shared(double fs, const RollParams& p, const RollTables& tables);
    // golden tests: use given (already dilated + smoothed) periodic tables
    void prepare_from_tables(double fs, const RollParams& p,
                             const std::vector<double>& surface_tab,
                             const std::vector<double>& rim_tab, double rim_dx);
    double process(double speed);
    // diagnostics (since prepare)
    double airborne_fraction() const;
    double landings_per_second() const;
    double contact_resonance_hz() const { return f_contact_; }
    double contact_resonance_used_hz() const { return f_contact_used_; }
    int oversample() const { return os_; }
private:
    void setup_contact(double fs, const RollParams& p, double* a_hertz);
    void reset_state();
    RollParams p_;
    SplinePtr surf_, rim_;
    Decimator dec_;
    int os_ = 4;
    double fs_ = 48000.0, fs_s_ = 0.0, k_ = 0.0, d0_ = 0.0, c0_ = 0.0, ce_ = 0.0;
    double f_contact_ = 0.0, f_contact_used_ = 0.0;
    int n_on_ = 1, on_len_ = 1, min_flight_ = 1, off_len_ = 0;
    bool started_ = false, was_off_ = false;
    double x_ = 0.0, y_ = 0.0, yd_ = 0.0, v_prev_ = 0.0, last_out_ = 0.0;
    long long steps_ = 0, airborne_ = 0, landings_ = 0, moving_samples_ = 0;
};

// ------------------------------------------------------------ sliding

struct SlideParams {
    double N0 = 1.0;          // N, normal force (weight)
    double m_p = 0.010;       // kg, contact mass (all tracks together)
    Material slider_mat, floor_mat;
    double wobble = 0.15;     // +- normal-force variation (< 6 Hz)
    int n_tracks = 1;         // asperity contact tracks under a flat face
    double sigma_lo = 1e-4;   // m, contact sizes, log-spaced over the tracks
    double sigma_hi = 1e-4;
    int oversample = 1;
    uint64_t seed = 0;
    std::size_t tile_points = std::size_t(1) << 18;   // 5.2 m surface tile
};

// Surface-profile slide exciter. process(): one SIGNED audio-rate velocity
// sample (m/s; back-and-forth strokes re-trace the same patch) in, one
// normal-force fluctuation sample (N, f - N) out.
class SlideExciter {
public:
    // one table per track (surface, contact size, seed); shareable
    static std::vector<SplinePtr> build_tables(const SlideParams& p,
                                               const SurfaceSpec& surface);
    void prepare(double fs, const SlideParams& p, const SurfaceSpec& surface);
    void prepare_shared(double fs, const SlideParams& p,
                        const std::vector<SplinePtr>& tables);
    // golden tests: one given periodic table per track
    void prepare_from_tables(double fs, const SlideParams& p,
                             const std::vector<std::vector<double>>& tabs);
    double process(double velocity);
    double lift_off_fraction() const;
    double landings_per_second() const;
private:
    struct Track {
        SplinePtr tab;
        double x = 0.0, y = 0.0, yt = 0.0;
        bool contact = true;
        int on_len = 0;
        double k_asp = 0.0;
    };
    void setup_common(double fs, const SlideParams& p);
    SlideParams p_;
    std::vector<Track> tracks_;
    Decimator dec_;
    Biquad wobble_lp_;
    Rng noise_{0};
    std::vector<double> pending_;   // landing pulses, ring buffer at fs_s
    std::size_t pend_pos_ = 0;
    int os_ = 1, n_on_ = 1;
    double fs_ = 48000.0, fs_s_ = 48000.0, wobble_rms_ = 1.0;
    double v_prev_ = 0.0, v_last_ = 0.0, last_out_ = 0.0;
    bool started_ = false;
    long long moving_steps_ = 0, flight_steps_ = 0, landings_ = 0;
};

}  // namespace pa
