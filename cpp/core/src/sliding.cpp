#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

#include "procaudio/exciters.h"

namespace pa {

static constexpr double kUpRatio = 10.0;     // upward following limit, x N/m_p
static constexpr double kRAsperity = 5e-4;   // m, asperity tip radius (landings)
static constexpr double kTOn = 3e-4;         // s, force build-up after a landing

// rms of white noise through the wobble low-pass at rate fs_s, measured once
// per rate (20 s of noise) and cached: it cost ~50 ms per voice
static double wobble_rms(double fs_s, const Biquad& lp) {
    static std::mutex mtx;
    static std::map<long long, double> cache;
    std::lock_guard<std::mutex> lock(mtx);
    long long key = std::llround(fs_s);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    Biquad probe = lp;
    Rng r(999);
    double ss = 0.0;
    const long warm = static_cast<long>(fs_s), cnt = static_cast<long>(20.0 * fs_s);
    for (long i = 0; i < warm + cnt; ++i) {
        double y = probe.process(r.normal());
        if (i >= warm) ss += y * y;
    }
    return cache[key] = std::sqrt(ss / cnt);
}

std::vector<SplinePtr> SlideExciter::build_tables(const SlideParams& p,
                                                  const SurfaceSpec& surface) {
    const int n = std::max(1, p.n_tracks);
    std::vector<SplinePtr> tabs;
    for (int j = 0; j < n; ++j) {
        // contact sizes log-spaced over the tracks (one size = a hump at
        // v / (2 pi sigma); a spread = the recordings' flat spectrum)
        double sig = (n == 1) ? p.sigma_lo
                              : p.sigma_lo * std::pow(p.sigma_hi / p.sigma_lo,
                                                      static_cast<double>(j) / (n - 1));
        uint64_t seed = (n == 1) ? p.seed : p.seed + 101ULL * static_cast<uint64_t>(j);
        auto sp = std::make_shared<PeriodicSpline>();
        sp->build(surface_tile(surface, p.tile_points, sig, seed), kDX);
        tabs.push_back(sp);
    }
    return tabs;
}

void SlideExciter::prepare(double fs, const SlideParams& p, const SurfaceSpec& surface) {
    prepare_shared(fs, p, build_tables(p, surface));
}

void SlideExciter::prepare_from_tables(double fs, const SlideParams& p,
                                       const std::vector<std::vector<double>>& tabs) {
    std::vector<SplinePtr> sp;
    for (const auto& t : tabs) {
        auto s = std::make_shared<PeriodicSpline>();
        s->build(t, kDX);
        sp.push_back(s);
    }
    prepare_shared(fs, p, sp);
}

void SlideExciter::prepare_shared(double fs, const SlideParams& p,
                                  const std::vector<SplinePtr>& tables) {
    setup_common(fs, p);
    const double k_asp = hertz_stiffness(p.slider_mat, p.floor_mat, kRAsperity);
    tracks_.assign(tables.size(), Track{});
    for (std::size_t j = 0; j < tables.size(); ++j) {
        tracks_[j].tab = tables[j];
        tracks_[j].k_asp = k_asp;
        tracks_[j].on_len = n_on_;
    }
}

void SlideExciter::setup_common(double fs, const SlideParams& p) {
    p_ = p;
    fs_ = fs;
    os_ = std::max(1, p.oversample);
    fs_s_ = fs * os_;
    n_on_ = std::max(1, static_cast<int>(std::lround(kTOn * fs_s_)));
    // shared normal-force wobble: white noise through a 6 Hz 2nd-order
    // Butterworth, normalized by its rms (measured once here)
    wobble_lp_.lowpass(6.0, fs_s_);
    noise_ = Rng(p.seed + 100);
    wobble_rms_ = wobble_rms(fs_s_, wobble_lp_);
    for (long i = 0; i < static_cast<long>(fs_s_); ++i)
        wobble_lp_.process(noise_.normal());   // settle the filter
    pending_.assign(std::size_t(1) << 14, 0.0);
    pend_pos_ = 0;
    dec_.setup(os_);
    started_ = false;
    v_prev_ = v_last_ = last_out_ = 0.0;
    moving_steps_ = flight_steps_ = landings_ = 0;
}

double SlideExciter::process(double velocity) {
    const double dt = 1.0 / fs_s_;
    const std::size_t nt = tracks_.size();
    const double inv_n = 1.0 / static_cast<double>(nt);
    const std::size_t pend_mask = pending_.size() - 1;
    for (int jj = 1; jj <= os_; ++jj) {
        double v = v_prev_ + (velocity - v_prev_) * static_cast<double>(jj) / os_;
        double a = started_ ? (v - v_last_) * fs_s_ : 0.0;
        v_last_ = v;
        double w = wobble_lp_.process(noise_.normal()) / wobble_rms_;
        double N = p_.N0 * std::max(1.0 + p_.wobble * w, 0.2);
        double Nj = N * inv_n, mpj = p_.m_p * inv_n, a_dn = Nj / mpj;
        bool moving = std::abs(v) > 1e-4;
        if (moving) ++moving_steps_;
        double total = 0.0;
        for (Track& t : tracks_) {
            t.x += v * dt;
            double z1, z2;
            double z = t.tab->eval(t.x, &z1, &z2);
            double zt = z1 * v, ztt = z2 * v * v + z1 * a;
            if (!started_) { t.y = z; t.yt = zt; t.contact = true; }
            double f = 0.0;
            if (!moving) {
                t.contact = true; t.y = z; t.yt = zt;
                total += Nj;
                continue;
            }
            if (t.contact) {
                if (ztt < -a_dn) {               // surface drops away: lift off
                    t.contact = false;
                    t.y = z; t.yt = zt;
                } else {
                    double a_up = kUpRatio * a_dn;
                    f = Nj + mpj * a_up * std::tanh(ztt / a_up);
                    if (t.on_len < n_on_) f *= static_cast<double>(t.on_len) / n_on_;
                    ++t.on_len;
                    total += f;
                    continue;
                }
            }
            // flight: pressed down by N, no contact force
            ++flight_steps_;
            t.yt -= a_dn * dt;
            t.y += t.yt * dt;
            if (t.y <= z) {                      // lands: Hertz micro-impact
                double v_rel = zt - t.yt;
                if (v_rel > 0.0) {
                    ++landings_;
                    double J = mpj * v_rel;
                    double tau = hertz_contact_time(mpj, t.k_asp, v_rel);
                    int L = std::max(2, static_cast<int>(std::lround(tau * fs_s_)));
                    L = std::min<int>(L, static_cast<int>(pending_.size()) - 1);
                    // half-sine pulse sin(pi (i + 1/2) / L), normalized to unit
                    // area: sum_i sin((i + 1/2) th) = 1 / sin(th / 2), th = pi / L.
                    // The sines come from a rotation recurrence: on soft floors
                    // (rug) a pulse is thousands of samples long and two sin()
                    // calls per sample made sliding cost 10x real time.
                    const double th = kPi / L;
                    const double cr = std::cos(th), sr = std::sin(th);
                    double sn = std::sin(0.5 * th), cn = std::cos(0.5 * th);
                    const double amp = J * fs_s_ * sn;   // J fs / sum
                    for (int i = 0; i < L; ++i) {
                        pending_[(pend_pos_ + static_cast<std::size_t>(i)) & pend_mask] += amp * sn;
                        const double s2 = sn * cr + cn * sr;
                        cn = cn * cr - sn * sr;
                        sn = s2;
                    }
                }
                t.contact = true; t.y = z; t.yt = zt;
                t.on_len = 0;                    // force builds up again
            }
        }
        started_ = true;
        total += pending_[pend_pos_];
        pending_[pend_pos_] = 0.0;
        pend_pos_ = (pend_pos_ + 1) & pend_mask;
        double out;
        if (dec_.push(total - N, out)) last_out_ = out;   // DC + slow wobble are silent
    }
    v_prev_ = velocity;
    return last_out_;
}

double SlideExciter::lift_off_fraction() const {
    return moving_steps_ ? static_cast<double>(flight_steps_) /
                               (static_cast<double>(moving_steps_) * tracks_.size())
                         : 0.0;
}

double SlideExciter::landings_per_second() const {
    return moving_steps_ ? landings_ / (moving_steps_ / fs_s_) / tracks_.size() : 0.0;
}

}  // namespace pa
