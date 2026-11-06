// Unit and real-time behaviour tests for procaudio-core (no framework).
// Golden comparisons against the Python prototype live in golden.py.
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

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

int main() {
    test_fft_and_spline();
    test_decimator_and_click();
    test_modal_decay();
    test_impact();
    test_block_invariance_and_edges();
    test_slide_box();
    test_drop_sequences();
    test_start_onsets();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
