// ContactVoice: one sounding object on one surface, the unit a plugin
// instance owns. Holds the object's and the floor's radiating modal banks
// and the exciters (impacts, rolling, sliding); everything is streaming, so
// the output does not depend on the host block size.
//
// Real-time contract: prepare*() allocate and build tables (call off the
// audio thread, or hand in cached tables); set_*(), hit(), drop() and
// process() do not allocate, except the bounded contact solve inside a hit.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "procaudio/bodies.h"
#include "procaudio/exciters.h"

namespace pa {

enum class Motion { None, Roll, Slide };

// Hunt-Crossley mu (s/m) giving restitution e_target for `object` landing
// on `floor` at v_in (proto bodies.calibrate_mu; offline, bisection).
double calibrate_mu(const Body& object, const Body& floor, double v_in,
                    double fs, double e_target);

// Stiffness for an object hitting a floor (Hertz; flat objects use a 3 mm
// edge radius, proto contact.render_contact default).
double impact_stiffness(const Body& object, const Body& floor);

// Restitution presets by material (proto bodies.RESTITUTION; cardboard
// added: soft, lossy).
double restitution_of(const Material& m);

// Effective mass at the contact point: Body::strike_mass, or the mass.
double strike_mass(const Body& b);
// Mass the contact solve sees: the mover's strike mass against a fixed
// target, else the reduced mass m1 m2 / (m1 + m2) (two free bodies).
double pair_effective_mass(const Body& mover, const Body& target);

// One impact of a drop sequence: time after the first touchdown, normal
// speed, effective mass of the striker.
struct DropHit { double t, v, m_eff; };

// Drop sequences (no audio; also used by tests):
//  ball: rebounds with restitution e, flight 2 v / g between hits
//  box:  lands on its face, then rocks on its bottom edges (Housner 1963:
//        theta'' = -p^2 sin(alpha - |theta|) sgn(theta), p^2 = 3 g / (4 R);
//        each edge impact keeps r = 1 - 1.5 sin^2(alpha) of the angular
//        velocity), each edge impact at v = 2 b |omega|, effective mass
//        I_corner / (2 b)^2 = M R^2 / (3 b^2)
std::vector<DropHit> drop_sequence(const Body& object, double v_drop, double e,
                                   double asymmetry = 0.25, int max_hits = 40,
                                   double v_min = 0.01);

class ContactVoice {
public:
    void prepare(double fs, const Body& object, const Body& floor);
    void prepare_roll(const RollParams& p, const SurfaceSpec& surface, const RimSpec& rim);
    void prepare_slide(const SlideParams& p, const SurfaceSpec& surface);
    void prepare_roll_shared(const RollParams& p, const RollTables& tables);
    void prepare_slide_shared(const SlideParams& p, const std::vector<SplinePtr>& tables);

    // Continuous motion. target is m/s (SIGNED for slides); it is smoothed
    // with a critically damped 2nd-order filter (no acceleration steps).
    void set_motion(Motion m) { motion_ = m; }
    Motion motion() const { return motion_; }
    void set_target_speed(double v) { target_ = v; }
    void set_smoothing(double tau_s);

    // Impact dissipation: mu = mu_ref * v_ref / v keeps the restitution
    // near-constant over speed (proto scenes/common.bounce_scene).
    void set_impact_dissipation(double mu_ref, double v_ref) { mu_ref_ = mu_ref; v_ref_ = v_ref; }
    // Restitution of drop bounce sequences; < 0: restitution_of(object material).
    void set_restitution(double e) { e_drop_ = e; }

    // One impact at normal speed v, `offset` samples into the next process()
    // call. m_eff <= 0: pair_effective_mass(object, target); a given m_eff
    // (a drop hit) is reduced against a free target. mu < 0: from the dissipation
    // setting. Returns the rebound speed of the contact solve.
    double impact(double v_normal, int offset, double mu = -1.0, double m_eff = -1.0);
    // Schedule a whole drop (ball bounces / box landing + rocking), starting
    // `offset` samples into the next process() call.
    void drop(double v_drop, int offset = 0);
    void cancel_scheduled() { n_sched_ = 0; }
    bool busy() const { return n_sched_ > 0; }

    // n output samples (mono, Pa-proxy * gain). speed: optional per-sample
    // speed track overriding the smoothed target (offline scenes).
    void process(float* out, int n, double gain = 1.0, const double* speed = nullptr);
    void process_stems(double* obj_out, double* floor_out, int n,
                       const double* speed = nullptr);

    const Body& object() const { return object_; }
    const Body& floor() const { return floor_; }
    RollExciter* roll() { return roll_.get(); }
    SlideExciter* slide() { return slide_.get(); }
    double last_contact_time() const { return last_contact_time_; }

private:
    struct Scheduled { long long at; double v, m_eff; };
    void push_scheduled(long long at, double v, double m_eff);

    double fs_ = 48000.0;
    Body object_, floor_;
    SoundingBody obj_snd_, floor_snd_;
    std::unique_ptr<RollExciter> roll_;
    std::unique_ptr<SlideExciter> slide_;
    Motion motion_ = Motion::None;
    ControlSmoother smooth_;
    double target_ = 0.0;
    std::vector<double> impact_buf_;   // scheduled impact force, ring buffer
    std::size_t impact_pos_ = 0;
    std::vector<Mode> contact_modes_;  // object + floor modes for the solve
    double k_impact_ = 0.0, m_eff_ = 0.0, last_contact_time_ = 0.0;
    // a free target radiates from its own center: its stem arrives later by
    // the center separation / c (listener direction averaged: / sqrt 3)
    double tgt_delay_ = 0.0;               // samples, 0 = none (fixed targets)
    double tgt_hist_[64] = {};
    unsigned tgt_pos_ = 0;
    double mu_ref_ = 0.3, v_ref_ = 1.0, e_drop_ = -1.0;
    long long now_ = 0;                // samples processed
    Scheduled sched_[64];              // pending drop hits (sorted by time)
    int n_sched_ = 0;
};

}  // namespace pa
