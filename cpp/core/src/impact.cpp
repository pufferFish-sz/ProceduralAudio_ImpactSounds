#include <algorithm>
#include <cmath>

#include "procaudio/exciters.h"

namespace pa {

static double hc_scalar(double x, double xdot, double k, double alpha, double lam) {
    if (x <= 0.0) return 0.0;
    double xa = std::pow(x, alpha);
    return std::max(0.0, k * xa + lam * xa * xdot);
}

ImpactResult simulate_impact_implicit(double m_s, double v_in, double k,
                                      double alpha, double lam,
                                      const std::vector<Mode>& modes, double fs,
                                      double t_contact_max, int oversample) {
    const int os = std::max(1, oversample);
    const double fs_s = fs * os, h = 1.0 / fs_s;
    const std::size_t N = modes.size();
    std::vector<double> w0sq(N), d(N), mn(N), D(N), q(N, 0.0), v(N, 0.0);
    double G = h * h / (4.0 * m_s), Gd = h / (2.0 * m_s);
    for (std::size_t i = 0; i < N; ++i) {
        double w0 = 2.0 * kPi * modes[i].freq;
        w0sq[i] = w0 * w0;
        d[i] = modes[i].decay;
        mn[i] = modes[i].mass;
        D[i] = 1.0 + h * d[i] + (h * h / 4.0) * w0sq[i];
        G += h * h / (4.0 * mn[i] * D[i]);
        Gd += h / (2.0 * mn[i] * D[i]);
    }
    double s = 0.0, vs = v_in, f_prev = 0.0;
    const long n_win = static_cast<long>(t_contact_max * fs_s);
    std::vector<double> f_hist, x_hist;
    f_hist.reserve(static_cast<std::size_t>(n_win));
    x_hist.reserve(static_cast<std::size_t>(n_win));
    std::vector<double> v_free(N), q_free(N);
    bool touched = false, stable = true;
    for (long it = 0; it < n_win; ++it) {
        double sum_qf = 0.0, sum_vf = 0.0;
        for (std::size_t i = 0; i < N; ++i) {
            double a_prev = -w0sq[i] * q[i] - 2.0 * d[i] * v[i] + f_prev / mn[i];
            double Cv = v[i] + 0.5 * h * a_prev - 0.5 * h * w0sq[i] * (q[i] + 0.5 * h * v[i]);
            v_free[i] = Cv / D[i];
            q_free[i] = q[i] + 0.5 * h * (v[i] + v_free[i]);
            sum_qf += q_free[i];
            sum_vf += v_free[i];
        }
        double s_free = s + h * vs - (h * h / (4.0 * m_s)) * f_prev;
        double vs_free = vs - (h / (2.0 * m_s)) * f_prev;
        double P = s_free - sum_qf, Pd = vs_free - sum_vf;
        double f = 0.0;
        if (P > 0.0) {
            auto g = [&](double fi) {
                return fi - hc_scalar(P - G * fi, Pd - Gd * fi, k, alpha, lam);
            };
            if (g(0.0) < 0.0) {
                // g is increasing in f: bracket, then Newton steps safeguarded
                // by bisection (the C++ counterpart of brentq)
                double lo = 0.0, hi = std::max(1.0, f_prev * 2.0);
                while (g(hi) < 0.0 && hi < 1e15) hi *= 4.0;
                f = std::min(std::max(f_prev, lo), hi);
                for (int iter = 0; iter < 100; ++iter) {
                    double gf = g(f);
                    if (gf < 0.0) lo = f; else hi = f;
                    double eps = 1e-6 * std::max(1.0, std::abs(f));
                    double dg = (g(f + eps) - gf) / eps;
                    double fn = (dg > 0.0) ? f - gf / dg : 0.5 * (lo + hi);
                    if (!(fn > lo && fn < hi)) fn = 0.5 * (lo + hi);
                    if (std::abs(fn - f) <= 1e-12 * std::max(1.0, std::abs(fn)) + 1e-9) {
                        f = fn;
                        break;
                    }
                    f = fn;
                }
            }
        }
        double sum_q = 0.0;
        for (std::size_t i = 0; i < N; ++i) {
            double v_new = v_free[i] + (0.5 * h / (mn[i] * D[i])) * f;
            q[i] = q[i] + 0.5 * h * (v[i] + v_new);
            v[i] = v_new;
            sum_q += q[i];
        }
        s = s_free - (h * h / (4.0 * m_s)) * f;
        vs = vs_free - (h / (2.0 * m_s)) * f;
        f_prev = f;
        f_hist.push_back(f);
        x_hist.push_back(s - sum_q);
        if (!std::isfinite(f) || !std::isfinite(sum_q)) {
            stable = false;
            break;
        }
        if (f > 0.0) touched = true;
        if (touched && f == 0.0 && (s - sum_q) < 0.0 && vs < 0.0) break;
    }
    ImpactResult r;
    r.stable = stable;
    r.v_out = vs;
    // contact statistics on the solver grid (as proto _contact_stats)
    long first_len = 0, n_contacts = 0;
    bool in = false, first_done = false;
    for (double x : x_hist) {
        bool c = x > 0.0;
        if (c && !in) ++n_contacts;
        if (c && !first_done) ++first_len;
        if (!c && in) first_done = true;
        in = c;
    }
    r.contact_time = first_len * h;
    r.n_contacts = static_cast<int>(n_contacts);
    for (double f : f_hist) r.f_max = std::max(r.f_max, f);
    // back to fs: block average (impulse preserving)
    std::size_t nb = (f_hist.size() + os - 1) / os;
    r.force.assign(nb, 0.0);
    for (std::size_t i = 0; i < f_hist.size(); ++i) r.force[i / os] += f_hist[i] / os;
    return r;
}

}  // namespace pa
