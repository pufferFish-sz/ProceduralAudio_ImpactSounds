#include "procaudio/catalog.h"

#include <cmath>
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

}  // namespace pa
