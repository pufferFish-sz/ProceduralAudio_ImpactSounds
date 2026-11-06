// Unit and real-time behaviour tests for procaudio-core (no framework).
// Golden comparisons against the Python prototype live in golden.py.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

#include "procaudio/body_desc.h"
#include "procaudio/catalog.h"
#include "procaudio/voice.h"

using namespace pa;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (cond) { ++g_pass; }                                       \
        else { ++g_fail; std::printf("FAIL %s:%d  ", __FILE__, __LINE__); \
               std::printf(__VA_ARGS__); std::printf("\n"); }         \
    } while (0)

static double max_step_db(const std::vector<double>& y) {
    double ss = 0.0, mx = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) {
        ss += y[i] * y[i];
        if (i) mx = std::max(mx, std::abs(y[i] - y[i - 1]));
    }
    double rms = std::sqrt(ss / y.size());
    return 20.0 * std::log10(mx / (rms + 1e-30) + 1e-30);
}

static void test_fft_and_spline() {
    const std::size_t n = 1024;
    std::vector<std::complex<double>> a(n);
    for (std::size_t i = 0; i < n; ++i) a[i] = std::sin(0.01 * i * i) + 0.3;
    auto b = a;
    fft(b, false);
    fft(b, true);
    double err = 0.0;
    for (std::size_t i = 0; i < n; ++i) err = std::max(err, std::abs(a[i] - b[i]));
    CHECK(err < 1e-12, "fft round trip error %g", err);

    // periodic spline of one sine period: value and derivatives
    const double dx = 1e-3, L = n * dx, k = 2.0 * kPi / L;
    std::vector<double> y(n);
    for (std::size_t i = 0; i < n; ++i) y[i] = std::sin(k * i * dx);
    PeriodicSpline s;
    s.build(y, dx);
    double e0 = 0, e1 = 0, e2 = 0;
    for (double x = -0.3; x < 2.0 * L; x += 0.0137) {
        double d1, d2, v = s.eval(x, &d1, &d2);
        e0 = std::max(e0, std::abs(v - std::sin(k * x)));
        e1 = std::max(e1, std::abs(d1 - k * std::cos(k * x)) / k);
        e2 = std::max(e2, std::abs(d2 + k * k * std::sin(k * x)) / (k * k));
    }
    CHECK(e0 < 1e-6 && e1 < 1e-5 && e2 < 1e-4, "spline errors %g %g %g", e0, e1, e2);
}

static void test_decimator_and_click() {
    Decimator d;
    d.setup(4);
    double out = 0.0;
    int outs = 0;
    for (int i = 0; i < 4000; ++i)
        if (d.push(1.0, out)) ++outs;
    CHECK(std::abs(out - 1.0) < 1e-9 && outs == 1000, "decimator DC %g outs %d", out, outs);

    // click filter magnitude vs the exact radiation law (compact dipole)
    const double fs = 48000.0, area = kPi * 0.03 * 0.03;
    ClickFilter cf;
    cf.setup(area, fs);
    // measure with a long sine at 500 Hz and 3 kHz
    for (double f : {200.0, 1000.0, 3000.0}) {
        ClickFilter c2 = cf;
        double peak = 0.0;
        for (int i = 0; i < 48000; ++i) {
            double y = c2.process(std::sin(2.0 * kPi * f * i / fs));
            if (i > 24000) peak = std::max(peak, std::abs(y));
        }
        double w = 2.0 * kPi * f, a = std::sqrt(area / kPi), w0 = kCAir / a;
        double K = kRhoAir * kCAir * std::sqrt(area / (2.0 * kPi)) / kRListen;
        double target = K * w / std::sqrt(4.0 * std::pow(w0, 4) + std::pow(w, 4));
        double err_db = 20.0 * std::log10(peak / target);
        CHECK(std::abs(err_db) < 0.5, "click filter |H| at %.0f Hz off by %.2f dB", f, err_db);
    }
}

static void test_modal_decay() {
    const double fs = 44100.0;
    ModalBank b;
    b.setup({{440.0, 3.0, 1.0, 0.05}}, fs);
    double e1 = 0, e2 = 0;
    for (int i = 0; i < static_cast<int>(2 * fs); ++i) {
        double y = b.process(i == 0 ? fs : 0.0);
        if (i >= fs * 0.2 && i < fs * 0.3) e1 = std::max(e1, std::abs(y));
        if (i >= fs * 1.2 && i < fs * 1.3) e2 = std::max(e2, std::abs(y));
    }
    double d = std::log(e1 / e2) / 1.0;
    CHECK(std::abs(d - 3.0) < 0.05, "modal decay %g vs 3", d);
}

static void test_impact() {
    const double fs = 44100.0;
    Body ball = solid_ball(0.03, "metal", "steel", "large");
    Body flr = floor_body("wood-floor");
    double k = impact_stiffness(ball, flr);
    std::vector<Mode> modes = ball.modes;
    modes.insert(modes.end(), flr.modes.begin(), flr.modes.end());
    auto r = simulate_impact_implicit(ball.mass, 1.0, k, 1.5, 0.3 * k, modes, fs);
    double tau = hertz_contact_time(ball.mass, k, 1.0);
    CHECK(r.stable && r.f_max > 0.0, "impact unstable");
    CHECK(-r.v_out > 0.0 && -r.v_out < 1.0, "rebound %g not in (0, 1)", -r.v_out);
    CHECK(r.contact_time > 0.5 * tau && r.contact_time < 2.0 * tau,
          "contact time %g vs Hertz %g", r.contact_time, tau);
    double J = 0.0;
    for (double f : r.force) J += f / fs;
    double J_exp = ball.mass * (1.0 - r.v_out);
    CHECK(std::abs(J - J_exp) / J_exp < 0.02, "impulse %g vs m dv %g", J, J_exp);
}

// the same voice, the same control track, different host block sizes
static std::vector<double> render_roll(int block, const char* mat, double radius) {
    const double fs = 44100.0;
    Body obj = solid_ball(radius, mat, "test", "x");
    Body flr = floor_body("varnished-wood-floor");
    ContactVoice v;
    v.prepare(fs, obj, flr);
    RollParams p;
    p.radius = radius;
    p.mass = obj.mass;
    p.k = 4.0 / 3.0 * effective_modulus(obj.mat, flr.mat) * std::sqrt(radius);
    p.tile_points = std::size_t(1) << 15;
    RimSpec rim;
    rim.ecc = 2e-6;
    v.prepare_roll(p, surface_spec("varnished-wood-floor"), rim);
    v.set_motion(Motion::Roll);
    v.set_smoothing(0.05);
    // events at absolute sample times that are block boundaries for every
    // tested size (multiples of 512), the impact at a sample offset inside
    const int t_speed = 9 * 512, t_impact = 52 * 512, impact_offset = 17;
    std::vector<double> out;
    std::vector<float> buf(static_cast<std::size_t>(block));
    const int n = 130 * 512;
    for (int done = 0; done < n; done += block) {
        if (done == t_speed) v.set_target_speed(0.5);
        if (done == t_impact) v.impact(0.3, impact_offset, 0.2);
        int m = std::min(block, n - done);
        v.process(buf.data(), m);
        out.insert(out.end(), buf.begin(), buf.begin() + m);
    }
    return out;
}

static void test_block_invariance_and_edges() {
    auto a = render_roll(64, "metal", 0.03), b = render_roll(512, "metal", 0.03);
    double diff = 0.0, ss = 0.0;
    std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        diff = std::max(diff, std::abs(a[i] - b[i]));
        ss += a[i] * a[i];
    }
    double rms = std::sqrt(ss / n);
    CHECK(a.size() == b.size() && diff == 0.0,
          "block 64 vs 512 differ: max diff %.3g (rms %.3g)", diff, rms);
    bool finite = true;
    for (double v : a) finite &= std::isfinite(v);
    CHECK(finite, "non-finite output");
    CHECK(rms > 0.0, "silent roll");
    double step = max_step_db(a);
    std::printf("  rolling steel ball: max sample step %.1f dB re rms\n", step);
    CHECK(step < 12.0, "hard edge in roll output: %.1f dB", step);
}

static void test_slide_box() {
    const double fs = 44100.0;
    Body box = box_body(0.3, 0.2, 0.2, "metal", 0.0008, 0.002, "steel", 0.03, 0.5);
    Body flr = floor_body("concrete-slab");
    SlideParams p;
    p.N0 = box.mass * kG;
    p.m_p = plate_contact_mass(box, 0.01);
    p.slider_mat = box.mat;
    p.floor_mat = flr.mat;
    p.wobble = 0.25;
    p.n_tracks = 6;
    p.sigma_lo = 60e-6;
    p.sigma_hi = 200e-6;
    p.oversample = 4;
    p.tile_points = std::size_t(1) << 16;
    add_contact_damping(box, 0.1);
    ContactVoice v;
    v.prepare(fs, box, flr);
    v.prepare_slide(p, surface_spec("concrete-slab"));
    v.set_motion(Motion::Slide);
    v.set_smoothing(0.1);
    std::vector<double> out;
    std::vector<float> buf(256);
    for (int i = 0; i < static_cast<int>(2.0 * fs); i += 256) {
        v.set_target_speed(i > static_cast<int>(0.1 * fs) ? 0.5 : 0.0);
        v.process(buf.data(), 256);
        out.insert(out.end(), buf.begin(), buf.end());
    }
    bool finite = true;
    double ss = 0.0;
    for (double y : out) { finite &= std::isfinite(y); ss += y * y; }
    CHECK(finite && ss > 0.0, "slide output non-finite or silent");
    CHECK(v.slide()->lift_off_fraction() < 0.01, "heavy box lifts off %g",
          v.slide()->lift_off_fraction());
    double step = max_step_db(out);
    std::printf("  dragged steel box: max sample step %.1f dB re rms\n", step);
    CHECK(step < 12.0, "hard edge in slide output: %.1f dB", step);
}

static void test_drop_sequences() {
    Body ball = solid_ball(0.03, "metal", "steel", "large");
    auto hb = drop_sequence(ball, 2.0, 0.7);
    bool dec = true;
    for (std::size_t i = 1; i < hb.size(); ++i)
        dec &= hb[i].v < hb[i - 1].v && hb[i].t > hb[i - 1].t &&
               (i < 2 || hb[i].t - hb[i - 1].t < hb[i - 1].t - hb[i - 2].t);
    CHECK(hb.size() > 5 && dec, "ball bounce sequence (%zu hits) not settling", hb.size());

    Body box = box_body(0.3, 0.2, 0.2, "cardboard", 0.004, 0.05, "cardboard");
    auto hx = drop_sequence(box, 2.0, 0.3);
    std::printf("  card box S dropped at 2 m/s:");
    for (const auto& h : hx) std::printf(" [%.3f s %.2f m/s %.0f g]", h.t, h.v, h.m_eff * 1e3);
    std::printf("\n");
    bool rock = hx.size() >= 3;
    for (std::size_t i = 2; i < hx.size(); ++i)
        rock &= hx[i].v < hx[i - 1].v && hx[i].t - hx[i - 1].t < hx[i - 1].t - hx[i - 2].t + 1e-9;
    CHECK(rock, "box rocking sequence not settling (%zu hits)", hx.size());
}

// a roll / slide that starts from rest must not start with a hit: the
// loudest 5 ms of the first 0.6 s vs the loudest 5 ms of the steady part
static void test_start_onsets() {
    const double fs = 48000.0;
    for (double tau : {0.02, 0.08}) {
        std::printf("  start smoothing %.0f ms, onset peak re steady peak:", tau * 1e3);
        double worst = -100.0;
        for (int o = 0; o < num_object_presets(); ++o) {
            auto v = make_preset_voice(o, o < 4 ? 0 : 2, fs);   // balls: wood, boxes: concrete
            v->set_motion(object_preset_is_ball(o) ? Motion::Roll : Motion::Slide);
            v->set_smoothing(tau);
            std::vector<double> ob(static_cast<std::size_t>(3.0 * fs)), fl(ob.size());
            v->set_target_speed(0.5);
            v->process_stems(ob.data(), fl.data(), static_cast<int>(ob.size()));
            auto peak5 = [&](double t0, double t1) {
                double best = 0.0;
                const std::size_t w = static_cast<std::size_t>(0.005 * fs);
                for (std::size_t i = static_cast<std::size_t>(t0 * fs);
                     i + w < static_cast<std::size_t>(t1 * fs); i += w) {
                    double ss = 0.0;
                    for (std::size_t j = i; j < i + w; ++j) ss += std::pow(ob[j] + fl[j], 2);
                    best = std::max(best, std::sqrt(ss / w));
                }
                return best;
            };
            double r = 20.0 * std::log10(peak5(0.0, 0.6) / peak5(1.0, 3.0));
            worst = std::max(worst, r);
            std::printf(" %s %+.1f", object_preset_name(o), r);
        }
        std::printf(" dB\n");
        if (tau == 0.08) CHECK(worst < 3.0, "start onset %.1f dB above steady state", worst);
    }
}

// ------------------------------------------------------------ body descriptions

// same set of modes; order-free, because degenerate pairs (equal frequency)
// can swap places on a last-bit difference in a dimension
static bool same_modes(const Body& a, const Body& b) {
    if (a.modes.size() != b.modes.size()) return false;
    auto key = [](std::vector<Mode> m) {
        std::sort(m.begin(), m.end(), [](const Mode& p, const Mode& q) {
            double fp = std::round(p.freq * 1e3), fq = std::round(q.freq * 1e3);
            return fp != fq ? fp < fq : p.mass < q.mass;
        });
        return m;
    };
    auto A = key(a.modes), B = key(b.modes);
    for (std::size_t i = 0; i < A.size(); ++i)
        if (std::abs(A[i].freq - B[i].freq) > 1e-9 * A[i].freq ||
            std::abs(A[i].mass - B[i].mass) > 1e-9 * A[i].mass ||
            std::abs(A[i].decay - B[i].decay) > 1e-9 * (A[i].decay + 1e-12))
            return false;
    return true;
}

static void test_body_desc() {
    // the catalog presets are special cases of a description
    Body p = make_preset_object(3);   // Steel ball L, r = 30 mm
    Body d = make_body({Shape::Ball, "metal", 0.060});
    CHECK(same_modes(p, d) && std::abs(p.mass - d.mass) < 1e-12,
          "Steel ball L preset != Ball/metal/60 mm description");
    BodyDesc card{Shape::Box, "cardboard", 0.30};
    Body pc = make_preset_object(4);  // Card box S (includes face-contact damping)
    Body dc = make_body(card);
    add_contact_damping(dc, 0.10);
    CHECK(same_modes(pc, dc) && std::abs(pc.mass - dc.mass) < 1e-12,
          "Card box S preset != Box/cardboard/30 cm description");
    // a finish applies to floors only (Panel, Ground): a "tiled" ball is a ball
    BodyDesc tiled_ball{Shape::Ball, "wood", 0.1};
    tiled_ball.finish = Finish::Tiled;
    BodyDesc tiled_ground{Shape::Ground, "concrete", 4.0};
    tiled_ground.finish = Finish::Tiled;
    CHECK(make_body(tiled_ball).mat.name == "wood" && std::string(texture_of(tiled_ball)) == "wood-floor",
          "finish on a ball was applied");
    CHECK(make_body(tiled_ground).mat.name == "ceramic" &&
          std::string(texture_of(tiled_ground)) == "tile-floor", "tiled ground not tiled");
    Body fw = floor_body("wood-floor");
    Body dw = make_body({Shape::Panel, "wood", 2.4, 0.022, Mount::BuiltIn});
    CHECK(same_modes(fw, dw) && dw.fixed, "wood-floor preset != built-in wood panel");

    // size: pitch ~ 1/size (geometric similarity), mass ~ size^3
    for (Shape s : {Shape::Ball, Shape::Stick, Shape::Box}) {
        Body a = make_body({s, "metal", 0.2}), b = make_body({s, "metal", 0.4});
        double fr = a.modes[0].freq / b.modes[0].freq, mr = b.mass / a.mass;
        double expect_m = s == Shape::Box ? 4.0 : 8.0;   // box wall stays fixed
        double expect_f = s == Shape::Box ? 4.0 : 2.0;   // box: plate f ~ h / a^2
        std::printf("  %-5s 20 -> 40 cm: f1 %.0f -> %.0f Hz (x1/%.2f), mass x%.2f\n",
                    shape_name(s), a.modes[0].freq, b.modes[0].freq, fr, mr);
        CHECK(std::abs(fr - expect_f) < 1e-6 * expect_f && std::abs(mr - expect_m) < 1e-6 * expect_m,
              "%s size scaling f x1/%g mass x%g", shape_name(s), fr, mr);
    }
    // every shape x material x mount builds and has a positive mass
    const char* mats[] = {"metal", "glass", "wood", "plastic", "rubber", "stone",
                          "cardboard", "concrete", "ceramic"};
    int built = 0;
    for (int s = 0; s < static_cast<int>(Shape::Count); ++s)
        for (const char* m : mats)
            for (int mt = 0; mt < static_cast<int>(Mount::Count); ++mt)
                for (double sz : {size_min(Shape(s)), 0.3, size_max(Shape(s))}) {
                    Body b = make_body({Shape(s), m, sz, 0.0, Mount(mt)});
                    bool ok = b.mass > 0.0;
                    for (const auto& md : b.modes) ok &= md.decay > 0.0 && md.decay < 2 * kPi * md.freq;
                    CHECK(ok, "%s %s %s %.2f m: bad body", shape_name(Shape(s)), m,
                          mount_name(Mount(mt)), sz);
                    ++built;
                }
    std::printf("  %d shape x material x mount x size bodies built\n", built);

    // effective masses
    Body ball = make_body({Shape::Ball, "wood", 0.10});
    CHECK(std::abs(pair_effective_mass(ball, ball) - 0.5 * ball.mass) < 1e-12,
          "two equal free balls: m_eff != M/2");
    Body slab = make_body({Shape::Ground, "concrete", 4.0});
    CHECK(pair_effective_mass(ball, slab) == ball.mass && slab.fixed, "ball on slab: m_eff != M");
    Body stick = make_body({Shape::Stick, "wood", 0.30, 0.012, Mount::Held, 1.0});
    CHECK(stick.modes.empty() && std::abs(stick.strike_mass - stick.mass / 3.0) < 1e-12,
          "held stick tip: rigid striker with M/3");
}

// restitution from the analytic Hunt-Crossley damping vs the target
static void test_pair_restitution() {
    const double fs = 48000.0;
    double worst_flores = 0.0, worst = 0.0;
    int low = 0, high = 0, ok_pairs = 0;
    std::printf("  achieved/target restitution, preset objects x floors (Flores alone | pair_mu;\n"
                "  * modes alone take more than the target, ^ contact damping cannot bring it down):\n");
    for (int o = 0; o < num_object_presets(); ++o) {
        Body obj = make_preset_object(o);
        std::printf("    %-13s", object_preset_name(o));
        for (int s = 0; s < num_surface_presets(); ++s) {
            Body flr = floor_body(surface_preset_slug(s));
            double e = pair_restitution(obj.mat, flr.mat);
            double k = impact_stiffness(obj, flr);
            std::vector<Mode> modes = obj.modes;
            modes.insert(modes.end(), flr.modes.begin(), flr.modes.end());
            auto r = simulate_impact_implicit(obj.mass, 1.0, k, 1.5, flores_mu(e, 1.0) * k,
                                              modes, fs);
            double ef = std::max(0.0, -r.v_out), ep = 0.0;
            double mu = pair_mu(obj, flr, e, fs, 1.0, &ep);
            // the most contact damping pair_mu can use
            double e_lossy = std::max(0.0, -simulate_impact_implicit(
                obj.mass, 1.0, k, 1.5, flores_mu(0.0, 1.0) * k, modes, fs).v_out);
            const char* tag = " ";
            worst_flores = std::max(worst_flores, std::abs(ef - e));
            if (std::abs(ep - e) > 0.03 && mu == 0.0 && ep < e) { ++low; tag = "*"; }
            else if (std::abs(ep - e) > 0.03 && e_lossy > e + 0.03) { ++high; tag = "^"; }
            else { ++ok_pairs; worst = std::max(worst, std::abs(ep - e)); }
            std::printf("  %.2f|%.2f/%.2f%s", ef, ep, e, tag);
        }
        std::printf("\n");
    }
    std::printf("  worst |e - e_target| over %d reachable pairs: Flores alone %.3f, pair_mu %.3f;"
                " %d pairs *, %d pairs ^\n", ok_pairs, worst_flores, worst, low, high);
    CHECK(worst < 0.03, "pair_mu misses the target restitution by %.3f", worst);
}

// a stick hits a ball, two equal balls clink: both bodies sound
static void test_pair_voices() {
    const double fs = 48000.0;
    struct Case { const char* name; BodyDesc a, b; };
    const Case cases[] = {
        {"held wood stick -> free wood ball 12 cm",
         {Shape::Stick, "wood", 0.60, 0.0, Mount::Held, 1.0}, {Shape::Ball, "wood", 0.12}},
        {"glass ball 6 cm -> same glass ball",
         {Shape::Ball, "glass", 0.06}, {Shape::Ball, "glass", 0.06}},
        // no audible modes: only the two clicks, which must not cancel
        {"glass marble 2.5 cm -> same marble",
         {Shape::Ball, "glass", 0.025}, {Shape::Ball, "glass", 0.025}},
        {"steel box 30 cm -> concrete slab (drop)",
         {Shape::Box, "metal", 0.30}, {Shape::Ground, "concrete", 4.0}},
        {"wood stick -> resting steel box",
         {Shape::Stick, "wood", 0.60, 0.0, Mount::Held, 1.0},
         {Shape::Box, "metal", 0.30, 0.0, Mount::Resting}},
    };
    for (const Case& c : cases) {
        auto v = make_pair_voice(c.a, c.b, fs);
        if (c.b.shape == Shape::Ground) v->drop(2.0); else v->impact(2.0, 0);
        std::vector<double> ob(static_cast<std::size_t>(fs)), tb(ob.size());
        v->process_stems(ob.data(), tb.data(), static_cast<int>(ob.size()));
        double eo = 0, et = 0, em = 0;
        bool finite = true;
        for (std::size_t i = 0; i < ob.size(); ++i) {
            eo += ob[i] * ob[i];
            et += tb[i] * tb[i];
            em += (ob[i] + tb[i]) * (ob[i] + tb[i]);
            finite &= std::isfinite(ob[i]) && std::isfinite(tb[i]);
        }
        double lo = 10 * std::log10(eo / ob.size() + 1e-30), lt = 10 * std::log10(et / tb.size() + 1e-30);
        double lm = 10 * std::log10(em / ob.size() + 1e-30);
        std::printf("  %-42s contact %.2f ms  object %5.1f  target %5.1f  mix %5.1f dB\n",
                    c.name, v->last_contact_time() * 1e3, lo, lt, lm);
        CHECK(finite && eo > 0.0 && et > 0.0, "%s: silent or non-finite stem", c.name);
        CHECK(lm > std::max(lo, lt) - 20.0, "%s: the stems cancel in the mix", c.name);
    }
}

// 1/3-octave band levels (dB) of x from 50 Hz to 16 kHz
static std::vector<double> third_octaves(const std::vector<double>& x, double fs) {
    std::size_t n = 1;
    while (n < x.size()) n <<= 1;
    std::vector<std::complex<double>> X(n);
    for (std::size_t i = 0; i < x.size(); ++i) X[i] = x[i];
    fft(X, false);
    std::vector<double> out;
    for (double fc = 50.0; fc < 16000.0; fc *= std::pow(2.0, 1.0 / 3.0)) {
        double lo = fc * std::pow(2.0, -1.0 / 6.0), hi = fc * std::pow(2.0, 1.0 / 6.0), e = 0.0;
        for (std::size_t k = static_cast<std::size_t>(lo * n / fs);
             k <= static_cast<std::size_t>(hi * n / fs) && k < n / 2; ++k)
            e += std::norm(X[k]);
        out.push_back(10.0 * std::log10(e + 1e-30));
    }
    return out;
}

// Slab A/B: the full 4 x 4 m concrete plate (all modes to 10 kHz) vs the
// slab model (modes below a cutoff + forced radiation). Same object, same
// impact, mix of object + floor. Picks how few slab modes are enough.
static void test_slab_ab() {
    const double fs = 48000.0;
    const std::size_t n = static_cast<std::size_t>(1.2 * fs);
    Body full = floor_body("concrete-slab");
    std::printf("  full concrete plate: %zu modes\n", full.modes.size());
    auto render = [&](const Body& obj, const Body& flr, std::vector<double>& mix,
                      std::vector<double>& fl) {
        ContactVoice v;
        v.prepare(fs, obj, flr);
        v.set_impact_dissipation(flores_mu(pair_restitution(obj.mat, flr.mat), 1.0), 1.0);
        v.impact(2.0, 0);
        std::vector<double> ob(n);
        fl.assign(n, 0.0);
        v.process_stems(ob.data(), fl.data(), static_cast<int>(n));
        mix.resize(n);
        for (std::size_t i = 0; i < n; ++i) mix[i] = ob[i] + fl[i];
    };
    double chosen_worst = 0.0;
    for (double fmax : {0.0, 2000.0, 4000.0, kSlabModeFmax}) {
        Body slab = slab_body("concrete", 4.0, 0.15, 0.3, fmax);
        double worst = 0.0, worst_floor_share = -100.0;
        for (int o = 0; o < num_object_presets(); ++o) {
            Body obj = make_preset_object(o);
            std::vector<double> ma, fa, mb, fb;
            render(obj, full, ma, fa);
            render(obj, slab, mb, fb);
            auto A = third_octaves(ma, fs), B = third_octaves(mb, fs);
            double top = *std::max_element(A.begin(), A.end());
            for (std::size_t i = 0; i < A.size(); ++i)
                if (A[i] > top - 30.0) worst = std::max(worst, std::abs(A[i] - B[i]));
            double ef = 0, em = 0;
            for (std::size_t i = 0; i < n; ++i) { ef += fa[i] * fa[i]; em += ma[i] * ma[i]; }
            worst_floor_share = std::max(worst_floor_share, 10 * std::log10(ef / em + 1e-30));
        }
        std::printf("  slab modes < %5.0f Hz: %3zu modes, worst 1/3-oct deviation of the mix "
                    "%.2f dB (full plate's floor stem <= %.1f dB re mix)\n",
                    fmax, slab.modes.size(), worst, worst_floor_share);
        if (fmax == kSlabModeFmax) chosen_worst = worst;
    }
    CHECK(chosen_worst < 1.0, "slab model (modes < %.0f Hz) deviates %.2f dB from the full plate",
          kSlabModeFmax, chosen_worst);
}

int main() {
    test_fft_and_spline();
    test_decimator_and_click();
    test_modal_decay();
    test_impact();
    test_block_invariance_and_edges();
    test_slide_box();
    test_drop_sequences();
    test_start_onsets();
    test_body_desc();
    test_pair_restitution();
    test_pair_voices();
    test_slab_ab();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
