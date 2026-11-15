#include "procaudio/catalog.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

namespace pa {

namespace {

struct ObjectPreset {
    const char* name;
    bool ball;
    // ball
    double radius;
    const char* material;
    const char* word;
    const char* size;
    double rim_ecc, rim_rough, rim_gamma;
    // box
    double a, b, c, wall, loss, cx, cy;
};

// scenes/roll.py BALLS and scenes/drag.py BOXES
const ObjectPreset kObjects[] = {
    {"Wood ball S", true, 0.020, "wood", "wooden", "small", 20e-6, 3e-6, 2.2, 0, 0, 0, 0, 0, 0, 0},
    {"Wood ball L", true, 0.050, "wood", "wooden", "large", 40e-6, 3e-6, 2.2, 0, 0, 0, 0, 0, 0, 0},
    {"Steel ball S", true, 0.010, "metal", "steel", "small", 1e-6, 0.1e-6, 2.4, 0, 0, 0, 0, 0, 0, 0},
    {"Steel ball L", true, 0.030, "metal", "steel", "large", 2e-6, 0.1e-6, 2.4, 0, 0, 0, 0, 0, 0, 0},
    {"Card box S", false, 0, "cardboard", "cardboard", "", 0, 0, 0, 0.30, 0.20, 0.20, 0.004, 0.05, 0.37, 0.43},
    {"Card box L", false, 0, "cardboard", "cardboard", "", 0, 0, 0, 0.60, 0.40, 0.40, 0.004, 0.05, 0.37, 0.43},
    {"Steel box S", false, 0, "metal", "sheet-steel", "", 0, 0, 0, 0.30, 0.20, 0.20, 0.0008, 0.002, 0.03, 0.5},
    {"Steel box L", false, 0, "metal", "sheet-steel", "", 0, 0, 0, 0.60, 0.40, 0.40, 0.001, 0.002, 0.03, 0.5},
};
constexpr int kNumObjects = sizeof(kObjects) / sizeof(kObjects[0]);

struct SurfacePreset { const char* name; const char* slug; };
const SurfacePreset kSurfaces[] = {
    {"Varnished wood", "varnished-wood-floor"},
    {"Wood floor", "wood-floor"},
    {"Concrete", "concrete-slab"},
    {"Tile", "tile-floor"},
    {"Rug", "rug"},
};
constexpr int kNumSurfaces = sizeof(kSurfaces) / sizeof(kSurfaces[0]);

constexpr double kBoxContactLoss = 0.10;   // scenes/drag.CONTACT_LOSS
constexpr std::size_t kTile = std::size_t(1) << 17;

int clamp_obj(int i) { return i < 0 ? 0 : (i >= kNumObjects ? kNumObjects - 1 : i); }
int clamp_surf(int i) { return i < 0 ? 0 : (i >= kNumSurfaces ? kNumSurfaces - 1 : i); }

RimSpec rim_of(const ObjectPreset& o) {
    RimSpec r;
    r.ecc = o.rim_ecc;
    r.rough_h_rms = o.rim_rough;
    r.rough_gamma = o.rim_gamma;
    return r;
}

RollParams roll_params(const Body& body, const Body& flr) {
    RollParams p;
    p.radius = body.contact_radius;
    p.mass = body.mass;
    p.k = 4.0 / 3.0 * effective_modulus(body.mat, flr.mat) * std::sqrt(p.radius);
    p.alpha = 1.5;
    p.zeta = 0.22;
    p.tile_points = kTile;
    return p;
}

SlideParams slide_params(const Body& body, const Body& flr) {
    SlideParams p;
    p.N0 = body.mass * kG;
    p.m_p = plate_contact_mass(body, 0.010);
    p.slider_mat = body.mat;
    p.floor_mat = flr.mat;
    p.wobble = 0.25;
    p.n_tracks = 6;
    p.sigma_lo = 60e-6;
    p.sigma_hi = 200e-6;
    p.oversample = 4;
    p.tile_points = kTile;
    return p;
}

struct Cache {
    std::mutex mtx;
    std::map<std::string, RollTables> roll;
    std::map<std::string, std::vector<SplinePtr>> slide;
    std::map<std::string, double> mu;
};

Cache& cache() {
    static Cache c;
    return c;
}

std::string key(const char* kind, int obj, int surf, double fs = 0.0) {
    return std::string(kind) + "|" + std::to_string(obj) + "|" + std::to_string(surf) + "|" +
           std::to_string(static_cast<long long>(fs));
}

// get-or-build without holding the lock while building
template <class T, class Build>
T get_or_build(std::map<std::string, T>& m, const std::string& k, Build build) {
    Cache& c = cache();
    {
        std::lock_guard<std::mutex> lock(c.mtx);
        auto it = m.find(k);
        if (it != m.end()) return it->second;
    }
    T v = build();
    std::lock_guard<std::mutex> lock(c.mtx);
    auto it = m.find(k);
    if (it != m.end()) return it->second;   // another thread was faster
    m[k] = v;
    return v;
}

}  // namespace

int num_object_presets() { return kNumObjects; }
const char* object_preset_name(int i) { return kObjects[clamp_obj(i)].name; }
bool object_preset_is_ball(int i) { return kObjects[clamp_obj(i)].ball; }
int num_surface_presets() { return kNumSurfaces; }
const char* surface_preset_name(int i) { return kSurfaces[clamp_surf(i)].name; }
const char* surface_preset_slug(int i) { return kSurfaces[clamp_surf(i)].slug; }

Body make_preset_object(int i) {
    const ObjectPreset& o = kObjects[clamp_obj(i)];
    if (o.ball) return solid_ball(o.radius, o.material, o.word, o.size);
    Body b = box_body(o.a, o.b, o.c, o.material, o.wall, o.loss, o.word, o.cx, o.cy);
    add_contact_damping(b, kBoxContactLoss);   // it rests on its face
    return b;
}

static void build_assets(int obj, int surf, double fs, std::unique_ptr<ContactVoice>* voice_out) {
    obj = clamp_obj(obj);
    surf = clamp_surf(surf);
    Body body = make_preset_object(obj);
    Body flr = floor_body(surface_preset_slug(surf));
    const SurfaceSpec& spec = surface_spec(surface_preset_slug(surf));
    Cache& c = cache();

    RollParams rp;
    RollTables rt;
    if (kObjects[obj].ball) {
        rp = roll_params(body, flr);
        rt = get_or_build(c.roll, key("roll", obj, surf),
                          [&] { return RollExciter::build_tables(rp, spec, rim_of(kObjects[obj])); });
    }
    SlideParams sp = slide_params(body, flr);
    auto st = get_or_build(c.slide, key("slide", 0, surf),
                           [&] { return SlideExciter::build_tables(sp, spec); });
    double mu = get_or_build(c.mu, key("mu", obj, surf, fs), [&] {
        return calibrate_mu(body, flr, 1.0, fs, restitution_of(body.mat));
    });
    if (!voice_out) return;
    auto v = std::make_unique<ContactVoice>();
    v->prepare(fs, body, flr);
    if (kObjects[obj].ball) v->prepare_roll_shared(rp, rt);
    v->prepare_slide_shared(sp, st);
    v->set_impact_dissipation(mu, 1.0);
    *voice_out = std::move(v);
}

std::unique_ptr<ContactVoice> make_preset_voice(int object, int surface, double fs) {
    std::unique_ptr<ContactVoice> v;
    build_assets(object, surface, fs, &v);
    return v;
}

bool preset_assets_ready(int object, int surface, double fs) {
    object = clamp_obj(object);
    surface = clamp_surf(surface);
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mtx);
    bool roll_ok = !kObjects[object].ball || c.roll.count(key("roll", object, surface));
    return roll_ok && c.slide.count(key("slide", 0, surface)) &&
           c.mu.count(key("mu", object, surface, fs));
}

void prewarm_presets(double fs, const std::atomic<bool>* stop) {
    for (int s = 0; s < kNumSurfaces; ++s)
        for (int o = 0; o < kNumObjects; ++o) {
            if (stop && stop->load()) return;
            build_assets(o, s, fs, nullptr);
        }
}

// ---------------------------------------------------------- any pair

BodyDesc preset_object_desc(int i) {
    const ObjectPreset& o = kObjects[clamp_obj(i)];
    BodyDesc d;
    d.material = o.material;
    if (o.ball) {
        d.shape = Shape::Ball;
        d.size = 2.0 * o.radius;
    } else {
        d.shape = Shape::Box;
        d.size = o.a;
        d.thickness = o.wall;
    }
    return d;
}

BodyDesc preset_surface_desc(int i) {
    BodyDesc d;
    d.mount = Mount::BuiltIn;
    const std::string slug = surface_preset_slug(i);
    if (slug == "concrete-slab" || slug == "tile-floor") {
        d.shape = Shape::Ground;
        d.material = "concrete";
        d.size = 4.0;
        if (slug == "tile-floor") d.finish = Finish::Tiled;
    } else {                                       // the 2.4 x 1.2 m wood floor bay
        d.shape = Shape::Panel;
        d.material = "wood";
        d.size = 2.4;
        if (slug == "varnished-wood-floor") d.finish = Finish::Polished;
        if (slug == "rug") d.finish = Finish::Carpeted;
    }
    return d;
}

double roll_radius_step(double radius) {
    double steps = std::round(6.0 * std::log2(std::max(radius, 1e-4) / 0.01));
    return 0.01 * std::pow(2.0, steps / 6.0);
}

namespace {

// Rim of a ball of this material and radius. Wood and steel reproduce the
// catalog balls (scenes/roll.py) at their radii; the rest are plausibility
// presets (smooth glass, molded plastic, rubber, rough stone).
RimSpec rim_for(const std::string& m, double r) {
    RimSpec s;
    auto ecc = [&](double e_ref, double r_ref, double p) { return e_ref * std::pow(r / r_ref, p); };
    if (m == "wood") { s.ecc = ecc(20e-6, 0.02, 0.756); s.rough_h_rms = 3e-6; s.rough_gamma = 2.2; }
    else if (m == "metal") { s.ecc = ecc(1e-6, 0.01, 0.631); s.rough_h_rms = 0.1e-6; s.rough_gamma = 2.4; }
    else if (m == "glass" || m == "ceramic") { s.ecc = ecc(2e-6, 0.02, 0.7); s.rough_h_rms = 0.05e-6; s.rough_gamma = 2.4; }
    else if (m == "plastic") { s.ecc = ecc(10e-6, 0.02, 0.7); s.rough_h_rms = 1e-6; s.rough_gamma = 2.2; }
    else if (m == "rubber") { s.ecc = ecc(50e-6, 0.02, 0.7); s.rough_h_rms = 5e-6; s.rough_gamma = 2.0; }
    else { s.ecc = ecc(30e-6, 0.02, 0.7); s.rough_h_rms = 5e-6; s.rough_gamma = 2.2; }
    return s;
}

std::string desc_key(const BodyDesc& d) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d|%s|%.6g|%.6g|%d|%.4g|%d", static_cast<int>(d.shape),
                  d.material.c_str(), d.size, d.thickness, static_cast<int>(d.mount), d.hit,
                  static_cast<int>(d.finish));
    return buf;
}

std::string pair_key(const BodyDesc& o, const BodyDesc& t, double fs) {
    return "pmu|" + desc_key(o) + "#" + desc_key(t) + "|" +
           std::to_string(static_cast<long long>(fs));
}

// the ball as the rolling tables see it: radius on the 1/6-octave grid
Body roll_ball(const Body& ball) {
    Body q = ball;
    double r = ball.contact_radius, rq = roll_radius_step(r);
    q.contact_radius = rq;
    q.mass = ball.mass * std::pow(rq / r, 3.0);
    return q;
}

// from the descriptions alone (no body build: desc_assets_ready is called
// on every parameter change)
std::string roll_key(const BodyDesc& od, const BodyDesc& td) {
    const double r = 0.5 * std::min(size_max(od.shape), std::max(size_min(od.shape), od.size));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6g", roll_radius_step(r));
    return "droll|" + od.material + "|" + buf + "|" + texture_of(td) + "|" +
           contact_material(td) + "|" + contact_material(od);
}

// Roughness (rms, m) of a slider's own contact face, by material: sanded
// wood, corrugated board, machined metal, molded plastic, glazed ceramic...
// Plausibility presets until recordings say otherwise.
double slider_roughness(const std::string& m) {
    if (m == "wood") return 6e-6;
    if (m == "cardboard") return 20e-6;
    if (m == "metal" || m == "plastic") return 1.5e-6;
    if (m == "glass") return 0.3e-6;
    if (m == "ceramic") return 1e-6;
    if (m == "rubber") return 8e-6;
    if (m == "stone") return 20e-6;
    if (m == "concrete") return 60e-6;
    return 10e-6;
}

// The surface a slider feels (descriptions only; the catalog presets keep
// the floor's own profile, as the Python reference does). Three things the
// floor profile alone gets wrong, each of which made slides on smooth
// floors sound like gravel (kurtosis 11-40 instead of ~3):
//  1. Both faces are rough. On a varnished floor (1.5 um) a sanded wooden
//     box (6 um) is the rough one: combined roughness sqrt(h_f^2 + h_s^2),
//     as for the rolling rims. It gives the continuous hiss.
//  2. A flat face bridges grooves and treads narrower than itself: only the
//     share width / face length of its contacts is over the feature. A
//     rounded slider (radius R) sinks into a groove of width w by at most
//     w^2 / (8 R).
//  3. "A little grit" is per metre of floor, not per contact track: the
//     tracks under one face share it. Polished and tiled floors are clean.
SurfaceSpec slide_surface(const BodyDesc& od, const Body& o, const BodyDesc& td, int n_tracks) {
    SurfaceSpec s = surface_spec(texture_of(td));
    const double hs = slider_roughness(contact_material(od));
    const double h2 = s.h_rms * s.h_rms + hs * hs;
    if (h2 > 0.0) {
        s.reson_share *= s.h_rms * s.h_rms / h2;   // the floor's texture keeps its power
        s.h_rms = std::sqrt(h2);
    }
    if (s.feat_period > 0.0) {
        if (o.contact_radius <= 0.0) {             // flat face: bridged (under 2 % left
            s.feat_period = 0.0;                   // for any face the plugin offers)
            s.feat_height = 0.0;
        } else if (s.feat_height < 0.0) {          // rounded slider over a groove
            const double sink = s.feat_width * s.feat_width / (8.0 * o.contact_radius);
            s.feat_height = -std::min(-s.feat_height, sink);
        }
    }
    s.grit_per_m /= std::max(1, n_tracks);
    const Finish fin = floor_finish(td);
    if (fin == Finish::Polished || fin == Finish::Tiled) s.grit_per_m *= 0.2;   // swept, hard floors
    return s;
}

std::string slide_key(const BodyDesc& od, const BodyDesc& td) {
    // what slide_surface depends on, from the descriptions alone
    const bool flat = od.shape != Shape::Ball && od.shape != Shape::Stick;
    const double size = std::min(size_max(od.shape), std::max(size_min(od.shape), od.size));
    char buf[96];
    // flat faces share one table; rounded: Ball radius (1/6-octave grid) / Stick edge.
    // The slider's material counts only through its roughness.
    std::snprintf(buf, sizeof(buf), "%d|%.6g|%.6g|%.3g", flat ? 1 : 0,
                  flat ? 0.0 : roll_radius_step(size),
                  od.shape == Shape::Stick ? od.thickness : 0.0,
                  slider_roughness(contact_material(od)));
    return std::string("dslide|") + texture_of(td) + "|" + buf + "|" +
           std::to_string(static_cast<int>(floor_finish(td)));
}

// A ball free to turn rolls and never slides (check_action): no slide tables.
bool needs_slide(const BodyDesc& od) {
    return !(od.shape == Shape::Ball && (od.mount == Mount::Free || od.mount == Mount::Resting));
}

struct DescBuild {
    Body o, t;
    double e = 0.0;
};

DescBuild desc_bodies(const BodyDesc& od, const BodyDesc& td) {
    DescBuild b{make_body(od), make_body(td), 0.0};
    // a box meeting a floor lands on its bottom face (catalog boxes do the same)
    if (od.shape == Shape::Box && b.t.fixed && od.mount != Mount::Resting)
        add_contact_damping(b.o, kBoxContactLoss);
    b.e = pair_restitution(b.o.mat, b.t.mat);
    return b;
}

void build_desc(const BodyDesc& od, const BodyDesc& td, double fs, bool motion,
                std::unique_ptr<ContactVoice>* voice_out) {
    DescBuild b = desc_bodies(od, td);
    Cache& c = cache();
    const bool ball = motion && od.shape == Shape::Ball && b.o.contact_radius > 0.0;
    RollParams rp;
    RollTables rt;
    if (ball) {
        Body q = roll_ball(b.o);
        rp = roll_params(q, b.t);
        rt = get_or_build(c.roll, roll_key(od, td), [&] {
            return RollExciter::build_tables(rp, surface_spec(texture_of(td)),
                                             rim_for(od.material, q.contact_radius));
        });
    }
    SlideParams sp = slide_params(b.o, b.t);
    std::vector<SplinePtr> st;
    const bool slides = motion && needs_slide(od);
    if (slides)
        st = get_or_build(c.slide, slide_key(od, td), [&] {
            return SlideExciter::build_tables(sp, slide_surface(od, b.o, td, sp.n_tracks));
        });
    double mu = get_or_build(c.mu, pair_key(od, td, fs),
                             [&] { return pair_mu(b.o, b.t, b.e, fs); });
    if (!voice_out) return;
    auto v = std::make_unique<ContactVoice>();
    v->prepare(fs, b.o, b.t);
    if (ball) v->prepare_roll_shared(rp, rt);
    if (slides) v->prepare_slide_shared(sp, st);
    v->set_impact_dissipation(mu, 1.0);
    v->set_restitution(b.e);
    *voice_out = std::move(v);
}

}  // namespace

std::unique_ptr<ContactVoice> make_desc_voice(const BodyDesc& object, const BodyDesc& target,
                                              double fs, bool motion) {
    std::unique_ptr<ContactVoice> v;
    build_desc(object, target, fs, motion, &v);
    return v;
}

bool desc_assets_ready(const BodyDesc& od, const BodyDesc& td, double fs, bool motion) {
    std::string rk;
    if (motion && od.shape == Shape::Ball) rk = roll_key(od, td);
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mtx);
    return (rk.empty() || c.roll.count(rk)) &&
           (!motion || !needs_slide(od) || c.slide.count(slide_key(od, td))) &&
           c.mu.count(pair_key(od, td, fs));
}

void prewarm_desc_presets(double fs, const std::atomic<bool>* stop) {
    for (int s = 0; s < kNumSurfaces; ++s)
        for (int o = 0; o < kNumObjects; ++o) {
            if (stop && stop->load()) return;
            build_desc(preset_object_desc(o), preset_surface_desc(s), fs, true, nullptr);
        }
}

}  // namespace pa
