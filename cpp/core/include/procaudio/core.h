// Constants, random numbers, materials, Hertz contact helpers and the modal
// resonator bank. Ports of proto/core/materials.py, impact.py (Hertz parts)
// and modal_bank.py.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace pa {

constexpr double kPi = 3.14159265358979323846;
constexpr double kG = 9.81;          // m/s^2
constexpr double kRhoAir = 1.2;      // kg/m^3
constexpr double kCAir = 343.0;      // m/s
constexpr double kRListen = 1.0;     // m, pressure-proxy listening distance
constexpr double kDX = 20e-6;        // m, surface profile grid (slide_profile.DX)
constexpr double kFMax = 16000.0;    // Hz, object modes above this are dropped

// ------------------------------------------------------------ random numbers

// PCG32: small, fast, deterministic across platforms. Not numpy's
// generator, so C++ and Python realizations of the same random surface
// differ; golden tests exchange tables instead (cpp/tests/golden.py).
class Rng {
public:
    explicit Rng(uint64_t seed = 0, uint64_t stream = 54);
    uint32_t next_u32();
    double uniform();                    // [0, 1)
    double normal();                     // N(0, 1), Box-Muller
    double exponential(double mean);
private:
    uint64_t state_ = 0, inc_ = 0;
    double spare_ = 0.0;
    bool has_spare_ = false;
};

// ------------------------------------------------------------ materials

struct Material {
    std::string name;
    double E = 0.0, rho = 0.0, nu = 0.3;
    double E_contact = 0.0;   // 0 -> E. Wood is indented across the grain.
    double tan_phi = 0.0;     // internal friction; 0 for surface-only entries
    double contact_E() const { return E_contact > 0.0 ? E_contact : E; }
};

// Throws std::out_of_range for an unknown name.
const Material& material(const std::string& name);

// Hertz contact modulus: 1/E* = (1-nu_a^2)/E_a + (1-nu_b^2)/E_b.
double effective_modulus(const Material& a, const Material& b);
// Sphere-on-flat stiffness k = 4/3 E* sqrt(R) (N/m^1.5, alpha = 1.5).
double hertz_stiffness(const Material& a, const Material& b, double radius);
// Lossless Hertz contact time 2.87 (m^2 / (k^2 v))^(1/5).
double hertz_contact_time(double m, double k, double v);

// ------------------------------------------------------------ modes

struct Mode {
    double freq = 0.0;    // Hz
    double decay = 0.0;   // 1/s, envelope ~ exp(-decay t)
    double gain = 1.0;    // output weight
    double mass = 0.05;   // kg, modal mass at the contact point
};

inline double material_damping(double freq, double tan_phi) {
    return kPi * freq * tan_phi;
}

// Exact-pole two-pole resonators, impulse invariant, driven open loop by a
// force: y[i] = a1 y[i-1] + a2 y[i-2] + b1 f[i-1]; output sum gain_n y_n.
class ModalBank {
public:
    void setup(const std::vector<Mode>& modes, double fs);
    double process(double force);
    // n samples at once (out is overwritten): the same per-mode recursion as
    // process(), with each mode's state kept in registers over the block and
    // 8 modes per step (SSE2). The modes' sum is formed in groups of 8, so it
    // can differ from process() in the last bits (~1e-16 relative); every mode
    // itself is bit-identical. ~4x faster for banks of hundreds of modes.
    void process_block(const double* force, double* out, int n);
    void reset();
    std::size_t size() const { return a1_.size(); }
private:
    std::vector<double> a1_, a2_, b1_, g_, y1_, y2_;
    double f_prev_ = 0.0;
};

}  // namespace pa
