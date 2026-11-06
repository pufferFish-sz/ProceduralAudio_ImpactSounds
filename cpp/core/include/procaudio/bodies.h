// Sounding bodies: shape primitives (proto/core/shapes.py), radiation
// (proto/core/radiation.py), the body catalog (proto/core/bodies.py and the
// scene factories) and SoundingBody = modes + radiation + rigid click
// (proto bodies.body_sound).
#pragma once

#include <string>
#include <vector>

#include "procaudio/core.h"
#include "procaudio/dsp.h"

namespace pa {

// ---- shapes: return modes and the total mass (kg) via `mass_out`
std::vector<Mode> bar_modes(double length, double width, double thickness,
                            const Material& mat, double strike, int n_modes,
                            double* mass_out);
std::vector<Mode> plate_ss_modes(double a, double b, double thickness,
                                 const Material& mat, double tan_phi, double x,
                                 double y, double f_max, double* mass_out);
std::vector<Mode> sphere_modes(double radius, const Material& mat, int l_max,
                               int overtones, double* mass_out);

// ---- radiation efficiency
double sigma_compact(double f, double area);
double coincidence_freq(double h, const Material& phys);
double sigma_plate(double f, double a, double b, double h, const Material& phys);
inline double velocity_to_pressure(double sigma, double area) {
    return kRhoAir * kCAir * std::sqrt(sigma * area / (2.0 * kPi)) / kRListen;
}

struct Radiator {
    bool plate = false;        // false: compact dipole
    double a = 0, b = 0, h = 0;
    Material phys;
};

struct Body {
    std::string slug, description;
    Material mat;              // contact material (Hertz)
    std::vector<Mode> modes;
    double mass = 0.0;         // kg
    double area = 0.0;         // m^2, radiating area
    double contact_radius = 0.0;  // m; <= 0 = flat
    bool fixed = false;        // floors: no rigid click
    Radiator rad;
    // rigid-body geometry for drops: a ball rebounds, a box rocks on its
    // edges (Housner). box_size = full extents a x b x c (c = height).
    bool is_ball = false;
    double box_a = 0.0, box_b = 0.0, box_c = 0.0;
};

// Extra loss into a contact: decay += pi f loss (bodies.with_contact_damping)
void add_contact_damping(Body& body, double loss);

// ---- catalog
Body floor_body(const std::string& slug);              // FLOOR_SPECS + derived
Body solid_ball(double radius, const std::string& material,
                const std::string& word, const std::string& size_word);
Body box_body(double a, double b, double c, const std::string& material,
              double wall, double loss, const std::string& word,
              double contact_x = 0.37, double contact_y = 0.43);
Body plank_body(double L, double w, double t, const std::string& material,
                double strike = 0.3);
// Plate-like sliders: contact mass from the panel point impedance
// Z = 8 sqrt(D rho h) at 1 kHz (scenes/drag.contact_mass); else `fallback`.
double plate_contact_mass(const Body& body, double fallback);

// ---- radiating body: driven by a force (N) sample by sample, returns a
// pressure proxy (Pa at 1 m). Modes: per-mode radiation gain folded in,
// output = velocity (difference of the displacement sum). Free bodies add
// the rigid "click" (ClickFilter on sign * F / M).
class SoundingBody {
public:
    void setup(const Body& body, double fs);
    double process(double force, double sign);
    void reset();
private:
    ModalBank bank_;
    ClickFilter click_;
    Biquad forced_lp_;          // plates: forced (drive-point) radiation
    double forced_gain_ = 0.0;  // rho_0 / (2 pi r rho_s); 0 for non-plates
    bool has_click_ = false;
    bool has_modes_ = false;
    double inv_mass_ = 0.0, fs_ = 48000.0, prev_ = 0.0;
};

}  // namespace pa
