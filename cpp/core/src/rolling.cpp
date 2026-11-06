#include <algorithm>
#include <cmath>

#include "procaudio/exciters.h"

namespace pa {

static void static_contact(double k, double alpha, double m, double& d0,
                           double& k_lin, double& f_c) {
    d0 = std::pow(m * kG / k, 1.0 / alpha);
    k_lin = alpha * k * std::pow(d0, alpha - 1.0);
    f_c = std::sqrt(k_lin / m) / (2.0 * kPi);
}

void RollExciter::setup_contact(double fs, const RollParams& p, double* a_hertz) {
    p_ = p;
    fs_ = fs;
    double d0_phys, k_lin, f_c;
    static_contact(p.k, p.alpha, p.mass, d0_phys, k_lin, f_c);
    f_contact_ = f_c;
    os_ = p.oversample > 0 ? p.oversample
                           : static_cast<int>(std::clamp(std::ceil(20.0 * f_c / fs), 4.0, 16.0));
    fs_s_ = fs * os_;
    // a contact resonance above fs_s / 20 is softened to fs_s / 20 (audibly
    // still rigid; keeps the semi-implicit step accurate and stable)
    k_ = p.k;
    double f_cap = fs_s_ / 20.0;
    if (f_c > f_cap)
        k_ *= (p.alpha == 1.0) ? std::pow(f_cap / f_c, 2.0) : std::pow(f_cap / f_c, 3.0);
    static_contact(k_, p.alpha, p.mass, d0_, k_lin, f_contact_used_);
    *a_hertz = std::sqrt(p.radius * d0_phys);
}

void RollExciter::reset_state() {
    c0_ = 2.0 * p_.zeta * std::sqrt(p_.mass * p_.alpha * k_);
    ce_ = (p_.alpha - 1.0) / 2.0;
    n_on_ = std::max(1, static_cast<int>(std::lround(p_.t_on * fs_s_)));
    on_len_ = n_on_;
    min_flight_ = static_cast<int>(2e-4 * fs_s_);
    dec_.setup(os_);
    started_ = was_off_ = false;
    off_len_ = 0;
    x_ = y_ = yd_ = v_prev_ = last_out_ = 0.0;
    steps_ = airborne_ = landings_ = moving_samples_ = 0;
}

void RollExciter::prepare_from_tables(double fs, const RollParams& p,
                                      const std::vector<double>& surface_tab,
                                      const std::vector<double>& rim_tab, double rim_dx) {
    auto surf = std::make_shared<PeriodicSpline>();
    surf->build(surface_tab, kDX);
    auto rim = std::make_shared<PeriodicSpline>();
    rim->build(rim_tab, rim_dx);
    prepare_shared(fs, p, RollTables{surf, rim});
}

RollTables RollExciter::build_tables(const RollParams& p, const SurfaceSpec& surface,
                                     const RimSpec& rim) {
    double d0_phys, k_lin, f_c;
    static_contact(p.k, p.alpha, p.mass, d0_phys, k_lin, f_c);
    const double a_hertz = std::sqrt(p.radius * d0_phys);   // contact half-width
    // surface: roughness averaged along a line contact; ride the asperity
    // tops (rigid circle), THEN average over the contact patch
    SurfaceSpec surf = surface;
    if (p.track_width > 0.0)
        surf.h_rms *= std::min(1.0, std::sqrt(surface.l_corr / p.track_width));
    std::vector<double> z = surface_tile(surf, p.tile_points, kDX, p.seed);
    std::vector<double> s = dilate_by_circle(z, p.radius, kDX);
    gaussian_smooth(s, std::max(a_hertz, kDX), kDX);
    auto surf_sp = std::make_shared<PeriodicSpline>();
    surf_sp->build(s, kDX);
    // rim: one circumference on a 2^k grid (spacing just under kDX)
    const double circ = 2.0 * kPi * p.radius;
    std::size_t n_c = next_pow2(std::max<std::size_t>(8, static_cast<std::size_t>(circ / kDX)));
    double dx_c = circ / static_cast<double>(n_c);
    std::vector<double> r = rim_profile(rim, n_c, p.radius, p.seed);
    gaussian_smooth(r, std::max(a_hertz, kDX), dx_c);
    auto rim_sp = std::make_shared<PeriodicSpline>();
    rim_sp->build(r, dx_c);
    return RollTables{surf_sp, rim_sp};
}

void RollExciter::prepare(double fs, const RollParams& p, const SurfaceSpec& surface,
                          const RimSpec& rim) {
    prepare_shared(fs, p, build_tables(p, surface, rim));
}

void RollExciter::prepare_shared(double fs, const RollParams& p, const RollTables& tables) {
    double a_hertz;
    setup_contact(fs, p, &a_hertz);
    surf_ = tables.surface;
    rim_ = tables.rim;
    reset_state();
}

double RollExciter::process(double speed) {
    speed = std::max(0.0, speed);
    if (speed > 1e-4) ++moving_samples_;
    const double dt = 1.0 / fs_s_, inv_m = 1.0 / p_.mass;
    for (int j = 1; j <= os_; ++j) {
        double v = v_prev_ + (speed - v_prev_) * static_cast<double>(j) / os_;
        x_ += v * dt;
        double s1, r1;
        double h = surf_->eval(x_, &s1) + rim_->eval(x_, &r1);
        double hd = (s1 + r1) * v;
        if (!started_) {                  // start resting in static contact
            y_ = h - d0_;
            yd_ = hd;
            started_ = true;
        }
        double dlt = h - y_, fi;
        if (dlt > 0.0) {
            double c = ce_ != 0.0 ? c0_ * std::pow(dlt > d0_ ? dlt : d0_, ce_) : c0_;
            if (on_len_ < n_on_) c *= static_cast<double>(on_len_) / n_on_;
            ++on_len_;
            fi = k_ * std::pow(dlt, p_.alpha) + c * (hd - yd_);
            if (fi < 0.0) fi = 0.0;
            if (was_off_ && off_len_ >= min_flight_) ++landings_;
            was_off_ = false;
            off_len_ = 0;
        } else {
            fi = 0.0;
            on_len_ = 0;
            ++airborne_;
            was_off_ = true;
            ++off_len_;
        }
        yd_ += dt * (fi * inv_m - kG);
        y_ += dt * yd_;
        ++steps_;
        double out;
        if (dec_.push(fi - p_.mass * kG, out)) last_out_ = out;
    }
    v_prev_ = speed;
    return last_out_;
}

double RollExciter::airborne_fraction() const {
    return steps_ ? static_cast<double>(airborne_) / static_cast<double>(steps_) : 0.0;
}

double RollExciter::landings_per_second() const {
    return moving_samples_ ? landings_ / (moving_samples_ / fs_) : 0.0;
}

}  // namespace pa
