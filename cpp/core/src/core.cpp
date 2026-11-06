#include "procaudio/core.h"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace pa {

// ------------------------------------------------------------ Rng (PCG32)

Rng::Rng(uint64_t seed, uint64_t stream) {
    inc_ = (stream << 1u) | 1u;
    next_u32();
    state_ += seed;
    next_u32();
}

uint32_t Rng::next_u32() {
    uint64_t old = state_;
    state_ = old * 6364136223846793005ULL + inc_;
    auto xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
    auto rot = static_cast<uint32_t>(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
}

double Rng::uniform() {
    // 53 random bits
    uint64_t hi = next_u32(), lo = next_u32();
    return static_cast<double>(((hi << 21) ^ lo) & ((1ULL << 53) - 1)) /
           static_cast<double>(1ULL << 53);
}

double Rng::normal() {
    if (has_spare_) {
        has_spare_ = false;
        return spare_;
    }
    double u1 = uniform(), u2 = uniform();
    if (u1 < 1e-300) u1 = 1e-300;
    double r = std::sqrt(-2.0 * std::log(u1));
    spare_ = r * std::sin(2.0 * kPi * u2);
    has_spare_ = true;
    return r * std::cos(2.0 * kPi * u2);
}

double Rng::exponential(double mean) {
    double u = uniform();
    return -mean * std::log(1.0 - u);
}

// ------------------------------------------------------------ materials

const Material& material(const std::string& name) {
    // proto/core/materials.py: PHYSICAL (E, rho, nu, E_contact) + MATERIALS
    // (tan_phi). Rounded engineering values, not citation-grade constants.
    static const std::map<std::string, Material> table = [] {
        std::map<std::string, Material> m;
        auto add = [&](const char* n, double E, double rho, double nu,
                       double tan_phi, double E_contact = 0.0) {
            m[n] = Material{n, E, rho, nu, E_contact, tan_phi};
        };
        add("metal", 200e9, 7850.0, 0.29, 0.0002);
        add("glass", 70e9, 2500.0, 0.22, 0.0008);
        add("wood", 11e9, 650.0, 0.30, 0.012, 1.0e9);
        add("plastic", 2.5e9, 1100.0, 0.37, 0.05);
        add("rubber", 0.01e9, 1100.0, 0.49, 0.15);
        add("stone", 50e9, 2700.0, 0.25, 0.004);
        add("cardboard", 1.7e9, 140.0, 0.30, 0.0);   // effective, 4 mm board
        add("concrete", 30e9, 2400.0, 0.20, 0.0);    // surface-only entries
        add("ceramic", 60e9, 2300.0, 0.22, 0.0);
        add("carpet", 0.2e6, 200.0, 0.30, 0.0);
        return m;
    }();
    return table.at(name);
}

double effective_modulus(const Material& a, const Material& b) {
    return 1.0 / ((1.0 - a.nu * a.nu) / a.contact_E() +
                  (1.0 - b.nu * b.nu) / b.contact_E());
}

double hertz_stiffness(const Material& a, const Material& b, double radius) {
    return 4.0 / 3.0 * effective_modulus(a, b) * std::sqrt(radius);
}

double hertz_contact_time(double m, double k, double v) {
    return 2.87 * std::pow(m * m / (k * k * std::max(v, 1e-3)), 0.2);
}

// ------------------------------------------------------------ ModalBank

void ModalBank::setup(const std::vector<Mode>& modes, double fs) {
    const double T = 1.0 / fs;
    const std::size_t n = modes.size();
    a1_.assign(n, 0.0); a2_.assign(n, 0.0); b1_.assign(n, 0.0);
    g_.assign(n, 0.0); y1_.assign(n, 0.0); y2_.assign(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const Mode& md = modes[i];
        double w0 = 2.0 * kPi * md.freq, d = md.decay;
        if (d >= w0) throw std::invalid_argument("overdamped mode");
        double wd = std::sqrt(w0 * w0 - d * d);
        double e = std::exp(-d * T);
        a1_[i] = 2.0 * e * std::cos(wd * T);
        a2_[i] = -(e * e);
        b1_[i] = T * e * std::sin(wd * T) / (md.mass * wd);
        g_[i] = md.gain;
    }
    f_prev_ = 0.0;
}

double ModalBank::process(double force) {
    double out = 0.0;
    const std::size_t n = a1_.size();
    for (std::size_t i = 0; i < n; ++i) {
        double y = a1_[i] * y1_[i] + a2_[i] * y2_[i] + b1_[i] * f_prev_;
        y2_[i] = y1_[i];
        y1_[i] = y;
        out += g_[i] * y;
    }
    f_prev_ = force;
    return out;
}

void ModalBank::reset() {
    std::fill(y1_.begin(), y1_.end(), 0.0);
    std::fill(y2_.begin(), y2_.end(), 0.0);
    f_prev_ = 0.0;
}

}  // namespace pa
