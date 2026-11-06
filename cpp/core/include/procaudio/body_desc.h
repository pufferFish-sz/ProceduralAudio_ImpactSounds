// Body descriptions: any sounding body from Shape + Material + Size + Mount.
//
// The preset catalog (catalog.h) hard-wires "object X on floor Y". A
// BodyDesc describes either side of a contact the same way, so a stick can
// hit a ball, two equal glasses can clink, and a floor is just a body whose
// Mount is BuiltIn. Pair physics (hardness, effective mass, restitution)
// is computed from the two descriptions, so N bodies need N descriptions,
// not N x N presets. Design: proto/docs/plugin_body_design.md.
#pragma once

#include <memory>
#include <string>

#include "procaudio/voice.h"

namespace pa {

enum class Shape { Ball, Stick, Box, Panel, Ground, Count };
enum class Mount { Free, Held, Resting, BuiltIn, Count };
// Surface finish: what the contact touches and how rough it is. Tiled and
// Carpeted cover the body (a tiled slab rings like the slab but is touched
// through ceramic, a rug sits on a wood floor); Polished is a smooth coat
// (varnish, lacquer, polish): same hardness, smoother to roll and slide on.
// The plugin offers Finish for the TARGET only ("Floor finish"): it is the
// surface the object hits, lands, rolls or slides on, and it applies to
// floors (Panel, Ground) only: see floor_finish().
enum class Finish { Bare, Polished, Tiled, Carpeted, Count };

const char* shape_name(Shape s);   // "Ball", "Stick", ...
const char* mount_name(Mount m);   // "Free", "Held", ...
const char* finish_name(Finish f); // "Bare", "Polished", ...

// Materials offered for bodies, in deck order (plugin dropdown).
int num_body_materials();
const char* body_material(int i);   // "metal", "glass", ...

// What the Size knob means for each shape (m):
//   Ball  diameter                 Stick  length
//   Box   longest side             Panel  longer span (b = a / 2)
//   Ground span (a = b): a slab on the earth; its sound barely depends on it
// thickness (m, 0 = shape/material default):
//   Stick cross-section (square), Box wall, Panel / Ground plate thickness;
//   ignored for Ball (solid).
// hit: where it is struck, 0 = center, 1 = end / edge (Stick, Box, Panel).
struct BodyDesc {
    Shape shape = Shape::Ball;
    std::string material = "wood";
    double size = 0.05;
    double thickness = 0.0;
    Mount mount = Mount::Free;
    double hit = 0.3;
    Finish finish = Finish::Bare;
};

// The finish that applies: floors (Panel, Ground) only. Other shapes are
// always Bare (a "tiled ball" is not a thing the plugin offers).
Finish floor_finish(const BodyDesc& d);
// Surface texture (profile.h SURFACE_PROFILES key) that a rolling or
// sliding object meets on this body: from the floor finish, else the material.
const char* texture_of(const BodyDesc& d);
// Contact material after the finish (Tiled: ceramic, Carpeted: carpet).
std::string contact_material(const BodyDesc& d);

double size_min(Shape s);
double size_max(Shape s);
// Shape/material default thickness for a body of this size (m).
double default_thickness(Shape s, const std::string& material, double size);

// Loss factor of a whole installed/built object (panel, box, slab) of this
// material: material damping plus joints and supports.
double installed_loss(const Material& m);

// Builds the body. Size is clamped to [size_min, size_max]; Ground is always
// BuiltIn. Throws std::out_of_range for an unknown material.
Body make_body(const BodyDesc& d);

// Ground: a floor slab on the earth (concrete, stone). Its many high modes are inaudible next
// to the object (the ground loads and damps them), so only modes below
// kSlabModeFmax are kept, plus the plate's forced radiation, which carries
// the contact "thud". See test_slab_ab() for the check against the full plate.
extern const double kSlabModeFmax;
// The slab itself, with an explicit mode cutoff (mode_fmax), for A/B tests.
Body slab_body(const std::string& material, double span, double thickness,
               double hit, double mode_fmax);

// ---------------------------------------------------------- pair physics

// Restitution of a material against a hard, heavy partner (rough presets).
double material_restitution(const Material& m);
// Pair restitution: the lossier body dominates, e = min(e_a, e_b).
double pair_restitution(const Material& a, const Material& b);
// Hunt-Crossley damping mu = lambda / k (s/m) that gives restitution e at
// impact speed v (Flores et al. 2011: lambda = 8 (1 - e) k / (5 e v)).
// Accounts for contact losses only, not energy that goes into the modes.
double flores_mu(double e, double v);
// Contact damping mu (s/m) that gives restitution e_target at speed v for
// this pair, counting what the two bodies' modes absorb. One probe solve
// without contact damping measures the modal loss e_modes; then
// flores_mu(e_target / e_modes), refined by at most 6 solves. If the modes
// alone already take more than e_target allows, mu = 0; if the compliance
// is in the modes (thin box walls), contact damping cannot reach e_target
// and the result stays above it. Each solve is one
// short contact (~ms for hundreds of modes): a build-time cost, not
// audio-thread work. e_achieved (optional) reports the result.
double pair_mu(const Body& o, const Body& t, double e_target, double fs, double v = 1.0,
               double* e_achieved = nullptr);

// A ContactVoice for `object` hitting `target` (impacts and drops; roll /
// slide need surface textures and come in a later phase). Restitution from
// pair_restitution(), dissipation from pair_mu().
std::unique_ptr<ContactVoice> make_pair_voice(const BodyDesc& object, const BodyDesc& target,
                                              double fs);

}  // namespace pa
