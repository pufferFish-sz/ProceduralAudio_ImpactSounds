#include "procaudio/body_desc.h"

#include <algorithm>
#include <cmath>

namespace pa {

// Chosen by test_slab_ab(): with modes below 6 kHz (280 of 475 for the
// 4 x 4 m, 15 cm slab) every catalog object's 2 m/s impact stays within
// 1 dB per 1/3 octave of the full plate. Lower cutoffs fail for small hard
// strikers (steel balls), whose sound is mostly the slab.
const double kSlabModeFmax = 6000.0;

namespace {

constexpr double kHandLoss = 0.02;    // grip damping of a held object (preset)
constexpr double kRestLoss = 0.10;    // resting on a face (scenes/drag.CONTACT_LOSS)

double clamp01(double x) { return std::min(1.0, std::max(0.0, x)); }

// Loss factor of a free, assembled object (boxes, loose boards). Metal and
// cardboard match the catalog boxes; the rest are ear-plausible presets.
double assembled_loss(const Material& m) {
    const std::string& n = m.name;
    if (n == "metal") return 0.002;
    if (n == "glass") return 0.002;
    if (n == "ceramic") return 0.003;
    if (n == "stone") return 0.01;
    if (n == "wood") return 0.02;
    if (n == "concrete") return 0.02;
    if (n == "cardboard" || n == "plastic") return 0.05;
    if (n == "rubber") return 0.15;
    return 0.3;   // carpet and anything soft
}

// Material for a solid body: surface-only entries (tan_phi = 0) get the
// assembled loss so they still decay.
Material solid_material(const std::string& name) {
    Material m = material(name);
    if (m.tan_phi <= 0.0) m.tan_phi = assembled_loss(m);
    return m;
}

// Contact point on a plate (fractions of a, b) for hit in [0, 1]. hit = 0.3
// reproduces the catalog's (0.37, 0.43); 0 is the center (every mode with a
// nodal line through it is silent), 1 is near a corner region.
void plate_point(double hit, double& x, double& y) {
    // written so that hit = 0.3 gives exactly the doubles 0.37 and 0.43: a
    // mode next to a nodal line is very sensitive to the last bit of x
    double h = clamp01(hit) / 0.3;
    x = (50.0 - 13.0 * h) / 100.0;
    y = (50.0 - 7.0 * h) / 100.0;
}

void apply_mount(Body& b, const BodyDesc& d) {
    switch (d.mount) {
        case Mount::Free:
        case Mount::Count:
            break;
        case Mount::Held:
            if (d.shape == Shape::Stick) {
                // proto bodies.wood_stick: a held stick is a rigid striker
                // pivoting about the hand at its far end. Its free-free bending
                // modes are too light at the tip and would absorb most of the
                // swing, which a gripped stick does not do (no grip model yet).
                double r = 0.5 + 0.5 * clamp01(d.hit);       // pivot distance / L
                b.strike_mass = std::min(b.mass, b.mass / (3.0 * r * r));
                b.modes.clear();
            } else {
                add_contact_damping(b, kHandLoss);
            }
            break;
        case Mount::Resting:
            b.fixed = true;                                    // the support carries it
            add_contact_damping(b, kRestLoss);
            break;
        case Mount::BuiltIn:
            b.fixed = true;
            break;
    }
}

}  // namespace

const char* shape_name(Shape s) {
    static const char* n[] = {"Ball", "Stick", "Box", "Panel", "Ground"};
    int i = static_cast<int>(s);
    return (i >= 0 && i < static_cast<int>(Shape::Count)) ? n[i] : "?";
}

const char* mount_name(Mount m) {
    static const char* n[] = {"Free", "Held", "Resting", "Built-in"};
    int i = static_cast<int>(m);
    return (i >= 0 && i < static_cast<int>(Mount::Count)) ? n[i] : "?";
}

const char* finish_name(Finish f) {
    static const char* n[] = {"Bare", "Polished", "Tiled", "Carpeted"};
    int i = static_cast<int>(f);
    return (i >= 0 && i < static_cast<int>(Finish::Count)) ? n[i] : "?";
}

static const char* const kBodyMaterials[] = {"metal", "glass", "wood", "plastic", "rubber",
                                             "stone", "cardboard", "concrete", "ceramic"};

int num_body_materials() {
    return static_cast<int>(sizeof(kBodyMaterials) / sizeof(kBodyMaterials[0]));
}

const char* body_material(int i) {
    i = std::min(std::max(i, 0), num_body_materials() - 1);
    return kBodyMaterials[i];
}

Finish floor_finish(const BodyDesc& d) {
    return (d.shape == Shape::Panel || d.shape == Shape::Ground) ? d.finish : Finish::Bare;
}

const char* texture_of(const BodyDesc& d) {
    switch (floor_finish(d)) {
        case Finish::Polished: return "varnished-wood-floor";   // 1.5 um, smooth coat
        case Finish::Tiled: return "tile-floor";
        case Finish::Carpeted: return "rug";
        default: break;
    }
    const std::string& m = d.material;
    if (m == "wood" || m == "cardboard") return "wood-floor";
    if (m == "metal") return "steel-plate-floor";
    if (m == "glass" || m == "ceramic") return "tile-floor";        // smooth, hard
    if (m == "plastic") return "varnished-wood-floor";              // smooth
    return "concrete-slab";                                         // concrete, stone, rubber
}

std::string contact_material(const BodyDesc& d) {
    const Finish f = floor_finish(d);
    if (f == Finish::Tiled) return "ceramic";
    if (f == Finish::Carpeted) return "carpet";
    return d.material;
}

double size_min(Shape s) {
    switch (s) {
        case Shape::Ball: return 0.005;
        case Shape::Stick: return 0.05;
        case Shape::Box: return 0.05;
        case Shape::Panel: return 0.2;
        case Shape::Ground: return 1.0;
        default: return 0.01;
    }
}

double size_max(Shape s) {
    switch (s) {
        case Shape::Ball: return 1.0;
        case Shape::Stick: return 3.0;
        case Shape::Box: return 2.0;
        case Shape::Panel: return 6.0;
        case Shape::Ground: return 20.0;
        default: return 1.0;
    }
}

double default_thickness(Shape s, const std::string& mat, double size) {
    switch (s) {
        case Shape::Ball:
            return 0.0;
        case Shape::Stick:
            return size / 25.0;                    // 60 cm stick -> 24 mm
        case Shape::Box:                           // wall
            if (mat == "cardboard") return 0.004;
            if (mat == "metal") return 0.001;
            if (mat == "wood") return 0.012;
            if (mat == "plastic") return 0.003;
            if (mat == "glass") return 0.004;
            return 0.005;
        case Shape::Panel:
            if (mat == "wood") return 0.022;       // FLOOR_SPECS wood-floor
            if (mat == "metal") return 0.006;      // steel-plate-floor
            if (mat == "concrete") return 0.15;
            if (mat == "stone") return 0.03;
            if (mat == "glass") return 0.006;
            if (mat == "plastic" || mat == "ceramic") return 0.01;
            if (mat == "cardboard") return 0.004;
            return 0.02;
        case Shape::Ground:
            return mat == "concrete" ? 0.15 : 0.10;
        default:
            return 0.01;
    }
}

double installed_loss(const Material& m) {
    // wood / metal / concrete: proto FLOOR_SPECS installed loss factors
    if (m.name == "wood") return 0.03;
    if (m.name == "metal") return 0.005;
    if (m.name == "concrete") return 0.05;
    return std::max(assembled_loss(m), 0.01);
}

Body slab_body(const std::string& material_name, double span, double thickness,
               double hit, double mode_fmax) {
    const Material& mat = material(material_name);
    double x, y;
    plate_point(hit, x, y);
    Body b;
    double M = 0.0;
    b.modes = mode_fmax > 0.0
                  ? plate_ss_modes(span, span, thickness, mat, installed_loss(mat), x, y,
                                   mode_fmax, &M)
                  : std::vector<Mode>{};
    b.mass = mat.rho * span * span * thickness;
    b.area = span * span;
    b.contact_radius = 0.0;
    b.fixed = true;
    b.mat = mat;
    // forced radiation p = rho_0 F / (2 pi r rho h) comes with rad.plate
    b.rad.plate = true;
    b.rad.a = span; b.rad.b = span; b.rad.h = thickness; b.rad.phys = mat;
    b.slug = material_name + "-slab";
    b.description = material_name + " slab on the ground, " +
                    std::to_string(static_cast<int>(std::lround(thickness * 1000))) + " mm";
    return b;
}

Body make_body(const BodyDesc& d) {
    const double size = std::min(size_max(d.shape), std::max(size_min(d.shape), d.size));
    const double h = d.thickness > 0.0 ? d.thickness
                                       : default_thickness(d.shape, d.material, size);
    const Material& mat = material(d.material);
    Body b;
    switch (d.shape) {
        case Shape::Ball: {
            const double r = 0.5 * size;
            b = solid_ball(r, d.material, d.material, "");
            if (mat.tan_phi <= 0.0) b.modes = sphere_modes(r, solid_material(d.material), 5, 2, nullptr);
            break;
        }
        case Shape::Stick: {
            const double t = std::min(h, 0.5 * size);
            const double strike = 0.5 - 0.5 * clamp01(d.hit);   // fraction from the end
            b = plank_body(size, t, t, d.material, strike);
            if (mat.tan_phi <= 0.0)
                b.modes = bar_modes(size, t, t, solid_material(d.material), strike, 8, nullptr);
            b.area = size * t;
            b.contact_radius = 0.5 * t;                         // rounded edge
            // free rigid bar hit off-center: 1/m = 1/M + d^2 / I, I = M L^2 / 12
            const double dh = clamp01(d.hit);
            b.strike_mass = b.mass / (1.0 + 3.0 * dh * dh);
            b.slug = d.material + "-stick";
            b.description = d.material + " stick";
            break;
        }
        case Shape::Box: {
            double x, y;
            plate_point(d.hit, x, y);
            b = box_body(size, size * 2.0 / 3.0, size * 2.0 / 3.0, d.material, h,
                         assembled_loss(mat), d.material, x, y);
            break;
        }
        case Shape::Panel: {
            double x, y, M = 0.0;
            plate_point(d.hit, x, y);
            const double a = size, bb = 0.5 * size;
            const double loss = d.mount == Mount::BuiltIn ? installed_loss(mat) : assembled_loss(mat);
            b.modes = plate_ss_modes(a, bb, h, mat, loss, x, y, 10000.0, &M);
            b.mass = M;
            b.mat = mat;
            b.area = a * bb;
            b.contact_radius = 0.0;
            b.rad.plate = true;
            b.rad.a = a; b.rad.b = bb; b.rad.h = h; b.rad.phys = mat;
            b.slug = d.material + "-panel";
            b.description = d.material + " panel";
            break;
        }
        case Shape::Ground:
        default:
            b = slab_body(d.material, size, h, d.hit, kSlabModeFmax);
            break;
    }
    switch (d.shape) {   // contact point -> center (radiation separation)
        case Shape::Ball: b.center_offset = 0.5 * size; break;
        case Shape::Stick: b.center_offset = 0.5 * clamp01(d.hit) * size + 0.5 * h; break;
        case Shape::Box: b.center_offset = 0.5 * b.box_c; break;
        default: b.center_offset = 0.5 * h; break;
    }
    // forced radiation: only where the panel bends (above its lowest mode)
    if (b.rad.plate && !b.modes.empty()) {
        double f1 = b.modes.front().freq;
        for (const Mode& m : b.modes) f1 = std::min(f1, m.freq);
        b.rad.forced_fmin = f1;
    }
    if (d.shape != Shape::Ground) apply_mount(b, d);
    if (floor_finish(d) == Finish::Tiled || floor_finish(d) == Finish::Carpeted) {
        b.mat = material(contact_material(d));   // touched through the covering
    }
    return b;
}

// ---------------------------------------------------------- pair physics

double material_restitution(const Material& m) {
    const std::string& n = m.name;
    if (n == "rubber") return 0.80;      // proto bodies.RESTITUTION
    if (n == "glass") return 0.75;
    if (n == "metal") return 0.70;
    if (n == "wood" || n == "plastic") return 0.60;
    if (n == "stone") return 0.50;
    if (n == "concrete") return 0.80;    // hard partners: rarely the lossy side
    if (n == "ceramic") return 0.75;
    if (n == "cardboard") return 0.30;
    return 0.10;                         // carpet and other soft surfaces
}

double pair_restitution(const Material& a, const Material& b) {
    return std::min(material_restitution(a), material_restitution(b));
}

double flores_mu(double e, double v) {
    e = std::min(0.99, std::max(0.01, e));
    return 8.0 * (1.0 - e) / (5.0 * e * std::max(v, 1e-3));
}

double pair_mu(const Body& o, const Body& t, double e_target, double fs, double v,
                double* e_achieved) {
    const double k = impact_stiffness(o, t), m = pair_effective_mass(o, t);
    std::vector<Mode> modes = o.modes;
    modes.insert(modes.end(), t.modes.begin(), t.modes.end());
    auto rebound = [&](double mu) {
        return std::max(0.0, -simulate_impact_implicit(m, v, k, 1.5, mu * k, modes, fs).v_out) / v;
    };
    // probe: what the modes alone take
    const double e_modes = rebound(0.0);
    double best_mu = 0.0, best_e = e_modes;
    if (e_modes > e_target) {
        // contact and modal losses multiply: e ~ e_modes * e_contact. e(mu)
        // is not monotonic for very soft contacts (rug), so keep the best try.
        auto take = [&](double mu, double e) {
            if (std::abs(e - e_target) < std::abs(best_e - e_target)) { best_mu = mu; best_e = e; }
        };
        double mu0 = flores_mu(e_target, v);
        take(mu0, rebound(mu0));
        double e_c = e_target / e_modes;
        for (int it = 0; it < 6 && std::abs(best_e - e_target) >= 0.01; ++it) {
            double mu = flores_mu(e_c, v), e = rebound(mu);
            take(mu, e);
            if (e <= 0.0) break;
            e_c = std::min(0.99, std::max(0.01, e_c * e_target / e));
        }
    }
    if (e_achieved) *e_achieved = best_e;
    return best_mu;
}

std::unique_ptr<ContactVoice> make_pair_voice(const BodyDesc& object, const BodyDesc& target,
                                              double fs) {
    Body o = make_body(object), t = make_body(target);
    // a box meeting a floor lands on its bottom face (catalog boxes do the same)
    if (object.shape == Shape::Box && t.fixed && object.mount != Mount::Resting)
        add_contact_damping(o, kRestLoss);
    const double e = pair_restitution(o.mat, t.mat);
    auto v = std::make_unique<ContactVoice>();
    v->prepare(fs, o, t);
    v->set_impact_dissipation(pair_mu(o, t, e, fs), 1.0);
    v->set_restitution(e);
    return v;
}

// ---------------------------------------------------------- what makes sense

const char* action_name(Action a) {
    static const char* n[] = {"Impact", "Drop", "Roll", "Slide", "None"};
    int i = static_cast<int>(a);
    return (i >= 0 && i < static_cast<int>(Action::Count)) ? n[i] : "?";
}

// The mount that applies: Ground is always Built-in.
static Mount mount_of(const BodyDesc& d) {
    return d.shape == Shape::Ground ? Mount::BuiltIn : d.mount;
}
// Moves on its own or by hand (Resting and Built-in bodies only sit there).
static bool moving(const BodyDesc& d) {
    const Mount m = mount_of(d);
    return m == Mount::Free || m == Mount::Held;
}
// Can be moved at all (Built-in bodies cannot; a Resting box can be pushed).
static bool movable(const BodyDesc& d) { return mount_of(d) != Mount::BuiltIn; }
// Free to turn: a ball that is not gripped or fixed.
static bool turns(const BodyDesc& d) {
    const Mount m = mount_of(d);
    return d.shape == Shape::Ball && (m == Mount::Free || m == Mount::Resting);
}
// Holds still against a scrape: anything not flying free.
static bool steady(const BodyDesc& d) { return mount_of(d) != Mount::Free; }

bool target_supports(const BodyDesc& t) {
    // a flat top (Box, Panel, Ground) that holds still: on the ground, built
    // in, resting on something, or held level (a tray)
    const bool flat = t.shape == Shape::Box || t.shape == Shape::Panel || t.shape == Shape::Ground;
    return flat && steady(t);
}

// Each motion's own test: "" when it can happen, else why not.
static const char* why_not(const BodyDesc& o, const BodyDesc& t, Action a) {
    const bool support = target_supports(t);
    switch (a) {
        case Action::Impact:
            if (!moving(o) && !moving(t)) return "nothing moves: both at rest";
            return "";
        case Action::Drop:
            if (!movable(o)) return "fixed objects can't fall";
            if (mount_of(o) == Mount::Held) return "held objects don't fall";
            if (mount_of(o) == Mount::Resting) return "already resting: can't fall";
            if (!support) return "nothing flat to land on";
            return "";
        case Action::Roll:
            if (o.shape != Shape::Ball) return "only balls roll";
            if (!movable(o)) return "fixed objects can't move";
            if (!turns(o)) return "a held ball can't turn";
            if (!support) return "nothing flat to roll on";
            return "";
        case Action::Slide:
            if (!movable(o)) return "fixed objects can't move";
            if (turns(o)) return "a free ball rolls instead";
            // pressed together by a hand, or by gravity on a flat support
            if (mount_of(o) == Mount::Held ? !steady(t) : !support)
                return mount_of(o) == Mount::Held ? "the target would fly off"
                                                  : "nothing flat to slide on";
            return "";
        default:
            return "";
    }
}

ActionCheck check_action(const BodyDesc& o, const BodyDesc& t, Action wanted) {
    if (wanted == Action::None) return {Action::None, ""};
    const char* why = why_not(o, t, wanted);
    if (!why[0]) return {wanted, ""};
    // the closest motion that can happen: a drop that cannot fall is a hit,
    // a roll that cannot turn slides, a slide that turns rolls
    Action alt = Action::None;
    if (wanted == Action::Drop) alt = Action::Impact;
    if (wanted == Action::Roll) alt = Action::Slide;
    if (wanted == Action::Slide) alt = Action::Roll;
    if (alt != Action::None && why_not(o, t, alt)[0]) alt = Action::None;
    return {alt, why};
}

}  // namespace pa
