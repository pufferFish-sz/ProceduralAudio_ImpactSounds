#include "procaudio/voice.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace pa {

double impact_stiffness(const Body& object, const Body& floor) {
    double r_obj = object.contact_radius > 0.0 ? object.contact_radius : 0.003;
    double inv_r = 1.0 / r_obj + (floor.contact_radius > 0.0 ? 1.0 / floor.contact_radius : 0.0);
    return hertz_stiffness(object.mat, floor.mat, 1.0 / inv_r);
}

double restitution_of(const Material& m) {
    if (m.name == "rubber") return 0.80;
    if (m.name == "glass") return 0.75;
    if (m.name == "metal") return 0.70;
    if (m.name == "wood" || m.name == "plastic") return 0.60;
    if (m.name == "stone") return 0.50;
    return 0.30;   // cardboard and other soft, lossy materials
}

double strike_mass(const Body& b) {
    return b.strike_mass > 0.0 ? b.strike_mass : b.mass;
}

double pair_effective_mass(const Body& mover, const Body& target) {
    double m1 = strike_mass(mover), m2 = strike_mass(target);
    if (target.fixed) return m1;     // object onto a floor / anchored body
    if (mover.fixed) return m2;      // a free body flies into a fixed "mover"
    return m1 * m2 / (m1 + m2);
}

static std::vector<Mode> joined_modes(const Body& a, const Body& b) {
    std::vector<Mode> m = a.modes;
    m.insert(m.end(), b.modes.begin(), b.modes.end());
    return m;
}

double calibrate_mu(const Body& object, const Body& floor, double v_in, double fs,
                    double e_target) {
    const double k = impact_stiffness(object, floor);
    const std::vector<Mode> modes = joined_modes(object, floor);
    auto err = [&](double mu) {
        ImpactResult r = simulate_impact_implicit(object.mass, v_in, k, 1.5, mu * k,
                                                  modes, fs, 0.01);
        return std::max(0.0, -r.v_out) / v_in - e_target;
    };
    double lo = 0.0, hi = 0.05;
    if (err(lo) <= 0.0) return 0.0;   // losses into the modes alone exceed it
    while (err(hi) > 0.0 && hi < 50.0) hi *= 2.0;
    for (int i = 0; i < 40 && hi - lo > 1e-5; ++i) {
        double mid = 0.5 * (lo + hi);
        if (err(mid) > 0.0) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

// ------------------------------------------------------------ drops

static int drop_sequence_into(const Body& b, double v_drop, double e, double asym,
                              int max_hits, double v_min, DropHit* out) {
    int n = 0;
    if (v_drop <= 0.0 || max_hits <= 0) return 0;
    out[n++] = {0.0, v_drop, b.mass};
    if (b.is_ball || b.box_c <= 0.0) {
        double t = 0.0, v = v_drop;
        while (n < max_hits) {
            v *= e;
            if (v < v_min) break;
            t += 2.0 * v / kG;                      // flight up and down
            out[n++] = {t, v, b.mass};
        }
        return n;
    }
    // box: rocking on its bottom edges about the narrower base side
    const double half_b = 0.5 * std::min(b.box_a, b.box_b), half_h = 0.5 * b.box_c;
    const double R = std::sqrt(half_b * half_b + half_h * half_h);
    const double alpha = std::atan2(half_b, half_h);
    const double p2 = 3.0 * kG / (4.0 * R);
    const double r = 1.0 - 1.5 * std::sin(alpha) * std::sin(alpha);
    if (r <= 0.0) return n;                          // too squat to rock
    const double m_edge = std::min(b.mass, b.mass * R * R / (3.0 * half_b * half_b));
    // landing asymmetry starts the rocking; never enough to tip it over
    double omega = asym * v_drop / R;
    const double omega_tip = std::sqrt(2.0 * p2 * (1.0 - std::cos(alpha)));
    omega = std::min(omega, 0.9 * omega_tip);
    double t = 0.0;
    const double dt = 2e-5;
    while (n < max_hits) {
        // free rocking from theta = 0 with |omega| until it returns to 0
        double th = 0.0, w = omega, tau = 0.0;
        do {
            double acc = -p2 * std::sin(alpha - th);   // restoring
            w += acc * dt;
            th += w * dt;
            tau += dt;
        } while (th > 0.0 && tau < 2.0);
        t += tau;
        double v_edge = 2.0 * half_b * std::abs(w);
        if (v_edge < v_min || t > 3.0) break;
        out[n++] = {t, v_edge, m_edge};
        omega = r * std::abs(w);                    // Housner restitution
    }
    return n;
}

std::vector<DropHit> drop_sequence(const Body& object, double v_drop, double e,
                                   double asymmetry, int max_hits, double v_min) {
    std::vector<DropHit> h(static_cast<std::size_t>(std::max(0, max_hits)));
    int n = drop_sequence_into(object, v_drop, e, asymmetry, max_hits, v_min, h.data());
    h.resize(static_cast<std::size_t>(n));
    return h;
}

// ------------------------------------------------------------ voice

void ContactVoice::prepare(double fs, const Body& object, const Body& floor) {
    fs_ = fs;
    object_ = object;
    floor_ = floor;
    obj_snd_.setup(object_, fs);
    floor_snd_.setup(floor_, fs);
    // 80 ms smoother: a roll/slide starts like a push, not a kick (a faster
    // start gives the profile force an acceleration transient that sounds
    // like an impact)
    smooth_.setup(0.08, fs);
    smooth_.reset(0.0);
    impact_buf_.assign(static_cast<std::size_t>(fs) + 1, 0.0);   // 1 s ahead
    impact_pos_ = 0;
    contact_modes_ = joined_modes(object_, floor_);
    k_impact_ = impact_stiffness(object_, floor_);
    m_eff_ = pair_effective_mass(object_, floor_);
    // Two free bodies are pushed apart, so their rigid-body clicks have
    // opposite signs; from one point they would cancel (two equal marbles:
    // silence). They radiate from their centers, d apart, so the target's
    // sound arrives d cos(theta) / c later; cos(theta) -> its rms 1/sqrt(3).
    tgt_delay_ = 0.0;
    if (!floor_.fixed) {
        double d = object_.center_offset + floor_.center_offset;
        tgt_delay_ = std::min(60.0, d / (kCAir * std::sqrt(3.0)) * fs);
    }
    std::fill(std::begin(tgt_hist_), std::end(tgt_hist_), 0.0);
    tgt_pos_ = 0;
    roll_.reset();
    slide_.reset();
    motion_ = Motion::None;
    now_ = 0;
    n_sched_ = 0;
}

void ContactVoice::prepare_roll(const RollParams& p, const SurfaceSpec& surface,
                                const RimSpec& rim) {
    roll_ = std::make_unique<RollExciter>();
    roll_->prepare(fs_, p, surface, rim);
}

void ContactVoice::prepare_slide(const SlideParams& p, const SurfaceSpec& surface) {
    slide_ = std::make_unique<SlideExciter>();
    slide_->prepare(fs_, p, surface);
}

void ContactVoice::prepare_roll_shared(const RollParams& p, const RollTables& tables) {
    roll_ = std::make_unique<RollExciter>();
    roll_->prepare_shared(fs_, p, tables);
}

void ContactVoice::prepare_slide_shared(const SlideParams& p,
                                        const std::vector<SplinePtr>& tables) {
    slide_ = std::make_unique<SlideExciter>();
    slide_->prepare_shared(fs_, p, tables);
}

void ContactVoice::set_smoothing(double tau_s) { smooth_.setup(tau_s, fs_); }

double ContactVoice::impact(double v_normal, int offset, double mu, double m_eff) {
    if (v_normal <= 0.0) return 0.0;
    if (mu < 0.0) mu = mu_ref_ * v_ref_ / std::max(v_normal, 1e-3);
    if (m_eff <= 0.0) {
        m_eff = m_eff_;
    } else if (!floor_.fixed) {
        double m2 = strike_mass(floor_);
        m_eff = m_eff * m2 / (m_eff + m2);
    }
    ImpactResult r = simulate_impact_implicit(m_eff, v_normal, k_impact_, 1.5,
                                              mu * k_impact_, contact_modes_, fs_);
    last_contact_time_ = r.contact_time;
    const std::size_t n = impact_buf_.size();
    std::size_t start = impact_pos_ + static_cast<std::size_t>(std::max(0, offset));
    for (std::size_t i = 0; i < r.force.size() && i < n - 1; ++i)
        impact_buf_[(start + i) % n] += r.force[i];
    return std::max(0.0, -r.v_out);
}

void ContactVoice::push_scheduled(long long at, double v, double m_eff) {
    if (n_sched_ >= 64) return;
    int i = n_sched_++;
    while (i > 0 && sched_[i - 1].at > at) {        // keep sorted by time
        sched_[i] = sched_[i - 1];
        --i;
    }
    sched_[i] = {at, v, m_eff};
}

void ContactVoice::drop(double v_drop, int offset) {
    DropHit hits[40];
    double e = e_drop_ >= 0.0 ? e_drop_ : restitution_of(object_.mat);
    int n = drop_sequence_into(object_, v_drop, e, 0.25, 40,
                               0.01, hits);
    const long long t0 = now_ + std::max(0, offset);
    for (int i = 0; i < n; ++i)
        push_scheduled(t0 + static_cast<long long>(std::llround(hits[i].t * fs_)), hits[i].v,
                       hits[i].m_eff);
}

void ContactVoice::process_stems(double* obj_out, double* floor_out, int n,
                                 const double* speed) {
    const std::size_t nb = impact_buf_.size();
    for (int i = 0; i < n; ++i) {
        while (n_sched_ > 0 && sched_[0].at <= now_) {   // due drop hits
            impact(sched_[0].v, 0, -1.0, sched_[0].m_eff);
            for (int j = 1; j < n_sched_; ++j) sched_[j - 1] = sched_[j];
            --n_sched_;
        }
        double v = speed ? speed[i] : smooth_.process(target_);
        double F = impact_buf_[impact_pos_];
        impact_buf_[impact_pos_] = 0.0;
        impact_pos_ = (impact_pos_ + 1) % nb;
        if (motion_ == Motion::Roll && roll_) F += roll_->process(std::abs(v));
        if (motion_ == Motion::Slide && slide_) F += slide_->process(v);
        obj_out[i] = obj_snd_.process(F, -1.0);
        double tg = floor_snd_.process(F, +1.0);
        if (tgt_delay_ > 0.0) {   // fractional delay, linear interpolation
            tgt_pos_ = (tgt_pos_ + 1) & 63u;
            tgt_hist_[tgt_pos_] = tg;
            const int di = static_cast<int>(tgt_delay_);
            const double fr = tgt_delay_ - di;
            tg = (1.0 - fr) * tgt_hist_[(tgt_pos_ - di) & 63u] +
                 fr * tgt_hist_[(tgt_pos_ - di - 1) & 63u];
        }
        floor_out[i] = tg;
        ++now_;
    }
}

void ContactVoice::process(float* out, int n, double gain, const double* speed) {
    double ob[256], fl[256];
    int done = 0;
    while (done < n) {
        int m = std::min(256, n - done);
        process_stems(ob, fl, m, speed ? speed + done : nullptr);
        for (int i = 0; i < m; ++i)
            out[done + i] = static_cast<float>(gain * (ob[i] + fl[i]));
        done += m;
    }
}

}  // namespace pa
