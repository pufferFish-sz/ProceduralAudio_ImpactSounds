#include "procaudio/bodies.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pa {

// ------------------------------------------------------------ shapes

static const double kBeamBetaL[] = {4.730041, 7.853205, 10.995608, 14.137165,
                                    17.278760, 20.420352, 23.561945, 26.703538};

static double beam_mode_shape(int n, double x_frac) {
    double bl = kBeamBetaL[n], bx = bl * x_frac;
    double sigma = (std::cosh(bl) - std::cos(bl)) / (std::sinh(bl) - std::sin(bl));
    return std::cosh(bx) + std::cos(bx) - sigma * (std::sinh(bx) + std::sin(bx));
}

std::vector<Mode> bar_modes(double length, double width, double thickness,
                            const Material& mat, double strike, int n_modes,
                            double* mass_out) {
    double area = width * thickness, I_over_A = thickness * thickness / 12.0;
    double M = mat.rho * area * length;
    double c = std::sqrt(mat.E / mat.rho);
    std::vector<Mode> modes;
    for (int n = 0; n < std::min(n_modes, 8); ++n) {
        double f = kBeamBetaL[n] * kBeamBetaL[n] / (2.0 * kPi * length * length) * c *
                   std::sqrt(I_over_A);
        double phi = beam_mode_shape(n, strike), phi2 = phi * phi;
        if (f > kFMax || phi2 < 1e-4) continue;
        modes.push_back({f, material_damping(f, mat.tan_phi), 1.0, M / phi2});
    }
    if (mass_out) *mass_out = M;
    return modes;
}

std::vector<Mode> plate_ss_modes(double a, double b, double thickness,
                                 const Material& mat, double tan_phi, double x,
                                 double y, double f_max, double* mass_out) {
    double M = mat.rho * a * b * thickness;
    double D = mat.E * thickness * thickness * thickness / (12.0 * (1.0 - mat.nu * mat.nu));
    double scale = kPi / 2.0 * std::sqrt(D / (mat.rho * thickness));
    int m_max = static_cast<int>(std::sqrt(f_max / scale) * a) + 1;
    int n_max = static_cast<int>(std::sqrt(f_max / scale) * b) + 1;
    std::vector<Mode> modes;
    for (int m = 1; m <= m_max; ++m)
        for (int n = 1; n <= n_max; ++n) {
            double f = scale * ((m / a) * (m / a) + (n / b) * (n / b));
            if (f > f_max) continue;
            double phi = 2.0 * std::sin(m * kPi * x) * std::sin(n * kPi * y);
            double phi2 = phi * phi;
            if (phi2 < 1e-3) continue;
            modes.push_back({f, material_damping(f, tan_phi), 1.0, M / phi2});
        }
    std::stable_sort(modes.begin(), modes.end(),
                     [](const Mode& p, const Mode& q) { return p.freq < q.freq; });
    if (mass_out) *mass_out = M;
    return modes;
}

// Lamb's free-sphere spheroidal equation times j_l(eta) j_l(xi) (no poles)
static double lamb_spheroidal(double eta, int l, double beta) {
    double xi = beta * eta;
    unsigned ul = static_cast<unsigned>(l);
    double jl = std::sph_bessel(ul, eta), jl1 = std::sph_bessel(ul + 1, eta);
    double xl = std::sph_bessel(ul, xi), xl1 = std::sph_bessel(ul + 1, xi);
    return (2.0 * (eta * eta * jl + (l - 1) * (l + 2) * (eta * jl1 - (l + 1) * jl))) * xi * xl1 +
           (-std::pow(eta, 4) / 2.0 + (l - 1) * (2 * l + 1) * eta * eta) * jl * xl +
           (eta * eta - 2.0 * l * (l - 1) * (l + 2)) * eta * jl1 * xl;
}

static std::vector<double> sphere_eta(int l, double nu, int n_roots) {
    double beta = std::sqrt((1.0 - 2.0 * nu) / (2.0 * (1.0 - nu)));
    std::vector<double> roots;
    const int N = 6000;
    double x0 = 0.2, x1 = 30.0, step = (x1 - x0) / (N - 1);
    double prev = lamb_spheroidal(x0, l, beta);
    for (int i = 1; i < N && static_cast<int>(roots.size()) < n_roots; ++i) {
        double x = x0 + i * step, g = lamb_spheroidal(x, l, beta);
        if ((prev < 0) != (g < 0)) {
            double lo = x - step, hi = x, glo = prev;
            for (int it = 0; it < 100; ++it) {
                double mid = 0.5 * (lo + hi), gm = lamb_spheroidal(mid, l, beta);
                if ((gm < 0) == (glo < 0)) { lo = mid; glo = gm; } else { hi = mid; }
            }
            roots.push_back(0.5 * (lo + hi));
        }
        prev = g;
    }
    return roots;
}

std::vector<Mode> sphere_modes(double radius, const Material& mat, int l_max,
                               int overtones, double* mass_out) {
    double M = mat.rho * 4.0 / 3.0 * kPi * radius * radius * radius;
    double c_t = std::sqrt(mat.E / (2.0 * (1.0 + mat.nu)) / mat.rho);
    std::vector<double> freqs;
    for (int l = 2; l <= l_max; ++l)
        for (double eta : sphere_eta(l, mat.nu, overtones))
            freqs.push_back(eta * c_t / (2.0 * kPi * radius));
    std::sort(freqs.begin(), freqs.end());
    std::vector<Mode> modes;
    for (std::size_t i = 0; i < freqs.size(); ++i) {
        if (freqs[i] > kFMax) break;
        modes.push_back({freqs[i], material_damping(freqs[i], mat.tan_phi), 1.0,
                         M * std::sqrt(static_cast<double>(i + 1))});
    }
    if (mass_out) *mass_out = M;
    return modes;
}

// ------------------------------------------------------------ radiation

double sigma_compact(double f, double area) {
    double a = std::sqrt(area / kPi);
    double ka4 = std::pow(2.0 * kPi * f / kCAir * a, 4.0);
    return ka4 / (4.0 + ka4);
}

double coincidence_freq(double h, const Material& phys) {
    double D = phys.E * h * h * h / (12.0 * (1.0 - phys.nu * phys.nu));
    return kCAir * kCAir / (2.0 * kPi) * std::sqrt(phys.rho * h / D);
}

double sigma_plate(double f, double a, double b, double h, const Material& phys) {
    // Maidanik average, simply supported baffled plate (radiation.sigma_plate)
    double fc = coincidence_freq(h, phys);
    double lc = kCAir / fc, S = a * b, P = 2.0 * (a + b);
    double s_cap = std::sqrt(a / lc) + std::sqrt(b / lc);
    double s;
    if (f >= fc) {
        s = (f > fc) ? std::pow(1.0 - fc / f, -0.5) : s_cap;
    } else {
        double al = std::sqrt(f / fc);
        double edge = P * lc / (4.0 * kPi * kPi * S) *
                      ((1.0 - al * al) * std::log((1.0 + al) / (1.0 - al)) + 2.0 * al) /
                      std::pow(1.0 - al * al, 1.5);
        double corner = 0.0;
        if (f < fc / 2.0)
            corner = 8.0 * lc * lc / (std::pow(kPi, 4) * S) * (1.0 - 2.0 * al * al) /
                     (al * std::sqrt(1.0 - al * al));
        s = corner + edge;
    }
    return std::min(s, s_cap);
}

static double sigma_of(const Body& b, double f) {
    return b.rad.plate ? sigma_plate(f, b.rad.a, b.rad.b, b.rad.h, b.rad.phys)
                       : sigma_compact(f, b.area);
}

void add_contact_damping(Body& body, double loss) {
    for (auto& m : body.modes) m.decay += kPi * m.freq * loss;
}

// ------------------------------------------------------------ catalog

Body floor_body(const std::string& slug) {
    struct Spec { const char* desc; const char* mat; double a, b, h, loss; };
    auto base = [](const std::string& s) -> Spec {
        if (s == "concrete-slab")
            return {"concrete slab on ground, 4 x 4 m, 15 cm thick", "concrete", 4.0, 4.0, 0.15, 0.05};
        if (s == "wood-floor")
            return {"wooden plank floor bay, 2.4 x 1.2 m, 22 mm boards", "wood", 2.4, 1.2, 0.022, 0.03};
        if (s == "steel-plate-floor")
            return {"steel deck plate walkway, 1 x 1 m, 6 mm", "metal", 1.0, 1.0, 0.006, 0.005};
        throw std::out_of_range("unknown floor " + s);
    };
    // derived floors (scenes/roll.rolling_floor): same plate, other contact
    std::string plate = slug, contact = "";
    if (slug == "tile-floor") { plate = "concrete-slab"; contact = "ceramic"; }
    if (slug == "tile-on-timber") { plate = "wood-floor"; contact = "ceramic"; }
    if (slug == "rug") { plate = "wood-floor"; contact = "carpet"; }
    if (slug == "varnished-wood-floor" || slug == "varnished-wood-floor-tight")
        plate = "wood-floor";
    Spec sp = base(plate);
    const Material& mat = material(sp.mat);
    Body b;
    b.slug = slug;
    b.description = sp.desc;
    double M = 0.0;
    b.modes = plate_ss_modes(sp.a, sp.b, sp.h, mat, sp.loss, 0.37, 0.43, 10000.0, &M);
    b.mass = M;
    b.area = sp.a * sp.b;
    b.contact_radius = 0.0;
    b.fixed = true;
    b.mat = contact.empty() ? mat : material(contact);
    b.rad.plate = true;
    b.rad.a = sp.a; b.rad.b = sp.b; b.rad.h = sp.h; b.rad.phys = mat;
    return b;
}

Body solid_ball(double radius, const std::string& material_name,
                const std::string& word, const std::string& size_word) {
    const Material& mat = material(material_name);
    Body b;
    double M = 0.0;
    b.modes = sphere_modes(radius, mat, 5, 2, &M);
    b.mass = M;
    b.mat = mat;
    b.area = kPi * radius * radius;
    b.contact_radius = radius;
    b.is_ball = true;
    b.slug = word + "-ball-" + size_word + "-r" +
             std::to_string(static_cast<int>(std::lround(radius * 1e3))) + "mm";
    b.description = "solid " + word + " ball, radius " +
                    std::to_string(radius * 100.0).substr(0, 4) + " cm";
    return b;
}

Body box_body(double a, double b_, double c, const std::string& material_name,
              double wall, double loss, const std::string& word,
              double contact_x, double contact_y) {
    const Material& mat = material(material_name);
    Body b;
    b.mass = mat.rho * wall * 2.0 * (a * b_ + a * c + b_ * c);
    b.modes = plate_ss_modes(a, b_, wall, mat, loss, contact_x, contact_y, 10000.0, nullptr);
    b.mat = mat;
    b.area = a * b_;
    b.contact_radius = 0.0;
    b.rad.plate = true;
    b.rad.a = a; b.rad.b = b_; b.rad.h = wall; b.rad.phys = mat;
    b.box_a = a; b.box_b = b_; b.box_c = c;
    auto cm = [](double v) { return std::to_string(static_cast<int>(std::lround(v * 100))); };
    b.slug = word + "-box-" + cm(a) + "x" + cm(b_) + "x" + cm(c) + "cm";
    b.description = word + " box, empty, sliding on its bottom panel";
    return b;
}

Body plank_body(double L, double w, double t, const std::string& material_name,
                double strike) {
    const Material& mat = material(material_name);
    Body b;
    double M = 0.0;
    b.modes = bar_modes(L, w, t, mat, strike, 8, &M);
    b.mass = M;
    b.mat = mat;
    b.area = L * w;
    b.contact_radius = 0.0;
    b.slug = material_name + "-plank";
    b.description = material_name + " plank lying flat on its broad face";
    return b;
}

double plate_contact_mass(const Body& body, double fallback) {
    if (!body.rad.plate || body.fixed) return fallback;
    const Material& ph = body.rad.phys;
    double h = body.rad.h;
    double D = ph.E * h * h * h / (12.0 * (1.0 - ph.nu * ph.nu));
    return 8.0 * std::sqrt(D * ph.rho * h) / (2.0 * kPi * 1000.0);
}

// ------------------------------------------------------------ SoundingBody

// Fixed pseudo-random +-1 per mode (Knuth multiplicative hash, top bit);
// same rule as proto bodies.mode_sign.
static double mode_sign(std::size_t i) {
    uint32_t h = static_cast<uint32_t>(static_cast<uint64_t>(i) * 2654435761ULL);
    return (h >> 31) ? -1.0 : 1.0;
}

void SoundingBody::setup(const Body& body, double fs) {
    fs_ = fs;
    std::vector<Mode> weighted;
    weighted.reserve(body.modes.size());
    for (std::size_t i = 0; i < body.modes.size(); ++i) {
        const Mode& m = body.modes[i];
        double sig = sigma_of(body, m.freq);
        double w = velocity_to_pressure(sig, body.area);
        Mode q = m;
        // each mode reaches the listener with its own sign (energy = sum of
        // modal powers), and loses the energy it radiates (radiation damping
        // rho c sigma S / 2M) -- proto bodies.body_sound
        q.gain = mode_sign(i) * w * std::sqrt(m.mass / body.mass);
        q.decay += kRhoAir * kCAir * sig * body.area / (2.0 * body.mass);
        weighted.push_back(q);
    }
    has_modes_ = !weighted.empty();
    if (has_modes_) bank_.setup(weighted, fs);
    // plates add their forced radiation p = rho_0 F / (2 pi r rho_s), coherent
    // with the force, low-passed at coincidence (proto bodies.forced_radiation)
    forced_gain_ = 0.0;
    if (body.rad.plate) {
        double rho_s = body.rad.phys.rho * body.rad.h;
        forced_gain_ = kRhoAir / (2.0 * kPi * kRListen * rho_s);
        forced_lp_.lowpass(std::min(coincidence_freq(body.rad.h, body.rad.phys), 0.45 * fs), fs);
    }
    has_click_ = !body.fixed;
    // rigid-body motion of the whole object radiates as a compact dipole,
    // even when its bending modes radiate like a plate
    if (has_click_) click_.setup(body.area, fs);
    inv_mass_ = 1.0 / body.mass;
    prev_ = 0.0;
}

double SoundingBody::process(double force, double sign) {
    double out = 0.0;
    if (has_modes_) {
        double y = bank_.process(force);
        out += (y - prev_) * fs_;
        prev_ = y;
    }
    if (has_click_) out += click_.process(sign * force * inv_mass_);
    if (forced_gain_ != 0.0) out += sign * forced_gain_ * forced_lp_.process(force);
    return out;
}

void SoundingBody::reset() {
    bank_.reset();
    prev_ = 0.0;
}

}  // namespace pa
