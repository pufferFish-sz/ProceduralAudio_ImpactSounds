#include "procaudio/scenes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "procaudio/voice.h"

namespace pa {

static std::string fmt(const char* f, double a) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, a);
    return buf;
}

static void apply_fade(std::vector<double>& y, double fs, double fade_s = 0.03) {
    std::size_t n = std::min(y.size(), static_cast<std::size_t>(fade_s * fs));
    for (std::size_t i = 0; i < n; ++i)
        y[y.size() - n + i] *= 0.5 * (1.0 + std::cos(kPi * static_cast<double>(i) / n));
}

// ------------------------------------------------------------ rolling balls

namespace {
struct Ball {
    double radius;
    const char* material;
    const char* word;
    const char* size;
    RimSpec rim;
};
}  // namespace

static RimSpec rim_spec(double ecc, double rough, double gamma) {
    RimSpec r;
    r.ecc = ecc;
    r.rough_h_rms = rough;
    r.rough_gamma = gamma;
    return r;
}

std::vector<RenderedScene> render_roll_spheres(double fs) {
    // scenes/roll.py BALLS, V_BALL, BALL_FLOOR, TIGHT_FLOOR, CONTACT, RAMP
    const std::vector<Ball> balls = {
        {0.020, "wood", "wooden", "small", rim_spec(20e-6, 3e-6, 2.2)},
        {0.050, "wood", "wooden", "large", rim_spec(40e-6, 3e-6, 2.2)},
        {0.010, "metal", "steel", "small", rim_spec(1e-6, 0.1e-6, 2.4)},
        {0.030, "metal", "steel", "large", rim_spec(2e-6, 0.1e-6, 2.4)},
    };
    const double DUR = 6.0, RAMP = 0.3, V_BALL = 0.5, zeta = 0.22, c_rr = 0.003;
    struct Run { const Ball* b; const char* floor; };
    std::vector<Run> runs;
    for (const auto& b : balls) runs.push_back({&b, "varnished-wood-floor"});
    for (const auto& b : balls)
        if (std::string(b.size) == "small") runs.push_back({&b, "varnished-wood-floor-tight"});

    std::vector<RenderedScene> out;
    const std::size_t n = static_cast<std::size_t>(DUR * fs);
    for (const Run& run : runs) {
        Body obj = solid_ball(run.b->radius, run.b->material, run.b->word, run.b->size);
        Body flr = floor_body(run.floor);
        ContactVoice voice;
        voice.prepare(fs, obj, flr);
        RollParams p;
        p.radius = run.b->radius;
        p.mass = obj.mass;
        p.k = 4.0 / 3.0 * effective_modulus(obj.mat, flr.mat) * std::sqrt(p.radius);
        p.alpha = 1.5;
        p.zeta = zeta;
        voice.prepare_roll(p, surface_spec(run.floor), run.b->rim);
        voice.set_motion(Motion::Roll);
        double v0 = V_BALL + c_rr * kG * (DUR - 0.1) / 2.0;
        std::vector<double> speed(n), ob(n), fl(n);
        for (std::size_t i = 0; i < n; ++i) {
            double t = static_cast<double>(i) / fs;
            double u = std::clamp((t - 0.1) / RAMP, 0.0, 1.0);
            double ramp = 0.5 - 0.5 * std::cos(kPi * u);
            speed[i] = ramp * std::max(v0 - c_rr * kG * (t - 0.1), 0.0);
        }
        voice.process_stems(ob.data(), fl.data(), static_cast<int>(n), speed.data());
        RenderedScene sc;
        sc.name = "roll_" + obj.slug + "_from-" + fmt("%.2f", v0) + "mps_on_" + run.floor;
        sc.object_stem = ob;
        sc.floor_stem = fl;
        sc.mix.resize(n);
        for (std::size_t i = 0; i < n; ++i) sc.mix[i] = ob[i] + fl[i];
        RollExciter* r = voice.roll();
        sc.facts.push_back("contact resonance " + fmt("%.0f", r->contact_resonance_hz()) +
                           " Hz, oversample " + std::to_string(r->oversample()) + "x");
        sc.facts.push_back("airborne " + fmt("%.1f", 100.0 * r->airborne_fraction()) +
                           " %, " + fmt("%.0f", r->landings_per_second()) + " landings/s");
        out.push_back(std::move(sc));
    }
    return out;
}

// ------------------------------------------------------------ box pulls

std::vector<RenderedScene> render_drag_boxes(double fs, const std::vector<double>* speed_in) {
    // scenes/drag.py BOXES, steady_pull, model C on concrete, BOX_TRIM_DB
    const double PULL_DUR = 8.0, PULL_V = 0.5, PULL_RAMP = 0.6, WOBBLE = 0.25;
    const double CONTACT_LOSS = 0.10;
    struct BoxSpec { double a, b, c; const char* mat; double wall, loss; const char* word;
                     double cx, cy, trim; };
    const std::vector<BoxSpec> boxes = {
        {0.30, 0.20, 0.20, "cardboard", 0.004, 0.05, "cardboard", 0.37, 0.43, -6.0},
        {0.60, 0.40, 0.40, "cardboard", 0.004, 0.05, "cardboard", 0.37, 0.43, -6.0},
        {0.30, 0.20, 0.20, "metal", 0.0008, 0.002, "sheet-steel", 0.03, 0.5, 0.0},
        {0.60, 0.40, 0.40, "metal", 0.001, 0.002, "sheet-steel", 0.03, 0.5, 0.0},
    };
    const std::size_t n = speed_in ? speed_in->size() : static_cast<std::size_t>(PULL_DUR * fs);
    // one pull, smooth start and stop, slow +-12 % hand variation (< 1.5 Hz)
    std::vector<double> speed(n);
    if (speed_in) {
        speed = *speed_in;
    } else {
        Biquad lp;
        lp.lowpass(1.5, fs);
        Rng rng(0);
        std::vector<double> w(n);
        for (std::size_t i = 0; i < static_cast<std::size_t>(fs); ++i) lp.process(rng.normal());
        double ss = 0.0;
        for (std::size_t i = 0; i < n; ++i) { w[i] = lp.process(rng.normal()); ss += w[i] * w[i]; }
        double sd = std::sqrt(ss / static_cast<double>(n));
        const double t0 = 0.1, t1 = PULL_DUR - 0.4;
        for (std::size_t i = 0; i < n; ++i) {
            double t = static_cast<double>(i) / fs;
            double up = std::clamp((t - t0) / PULL_RAMP, 0.0, 1.0);
            double dn = std::clamp((t1 - t) / PULL_RAMP, 0.0, 1.0);
            double env = (0.5 - 0.5 * std::cos(kPi * up)) * (0.5 - 0.5 * std::cos(kPi * dn));
            speed[i] = PULL_V * env * (1.0 + 0.12 * w[i] / sd);
        }
    }
    double dist = 0.0;
    for (double v : speed) dist += v / fs;

    std::vector<RenderedScene> out;
    const Body flr = floor_body("concrete-slab");
    for (const BoxSpec& bs : boxes) {
        Body box = box_body(bs.a, bs.b, bs.c, bs.mat, bs.wall, bs.loss, bs.word, bs.cx, bs.cy);
        SlideParams p;
        p.N0 = box.mass * kG;
        p.m_p = plate_contact_mass(box, 0.010);
        p.slider_mat = box.mat;
        p.floor_mat = flr.mat;
        p.wobble = WOBBLE;
        p.n_tracks = 6;
        p.sigma_lo = 60e-6;
        p.sigma_hi = 200e-6;
        p.oversample = 4;
        add_contact_damping(box, CONTACT_LOSS);
        ContactVoice voice;
        voice.prepare(fs, box, flr);
        voice.prepare_slide(p, surface_spec("concrete-slab"));
        voice.set_motion(Motion::Slide);
        std::vector<double> ob(n), fl(n);
        voice.process_stems(ob.data(), fl.data(), static_cast<int>(n), speed.data());
        RenderedScene sc;
        sc.name = "drag_" + box.slug + "_pulled-" + fmt("%.1f", dist) + "m-at-" +
                  fmt("%g", PULL_V) + "mps_on_concrete-slab";
        sc.object_stem = ob;
        sc.floor_stem = fl;
        sc.mix.resize(n);
        for (std::size_t i = 0; i < n; ++i) sc.mix[i] = ob[i] + fl[i];
        sc.trim_db = bs.trim;
        sc.facts.push_back("box " + fmt("%.2f", box.mass) + " kg, contact mass " +
                           fmt("%.1f", p.m_p * 1e3) + " g, 6 asperity tracks 60-200 um");
        sc.facts.push_back("lift-off " + fmt("%.1f", 100.0 * voice.slide()->lift_off_fraction()) +
                           " % per track");
        out.push_back(std::move(sc));
    }
    return out;
}

// ------------------------------------------------------------ ball drop

std::vector<RenderedScene> render_ball_drop(double fs) {
    // scenes/common.bounce_scene: rubber balls dropped from 1 m on a wooden
    // floor, mu calibrated for restitution 0.8 at the first impact, mu * v
    // held constant afterwards
    const double h0 = 1.0, e_rubber = 0.80, tail = 1.0, v_min = 0.15, t_max = 8.0;
    std::vector<RenderedScene> out;
    const Body flr = floor_body("wood-floor");
    for (double radius : {0.015, 0.040}) {
        Body ball;
        double M = 0.0;
        const Material& rubber = material("rubber");
        ball.modes = sphere_modes(radius, rubber, 5, 2, &M);
        ball.mass = M;
        ball.mat = rubber;
        ball.area = kPi * radius * radius;
        ball.contact_radius = radius;
        ball.slug = std::string("rubber-ball-") + (radius < 0.02 ? "small" : "large") + "-r" +
                    std::to_string(static_cast<int>(std::lround(radius * 1e3))) + "mm";
        double v = std::sqrt(2.0 * kG * h0), t = std::sqrt(2.0 * h0 / kG), v0 = v;
        double mu0 = calibrate_mu(ball, flr, v0, fs, e_rubber);
        ContactVoice voice;
        voice.prepare(fs, ball, flr);
        // impact times are found as the render goes (each rebound sets the next)
        std::vector<double> ob, fl;
        std::size_t pos = 0;
        int bounces = 0;
        double next_t = t, t_end = t_max + tail;
        const int block = 256;
        double obuf[block], fbuf[block];
        while (static_cast<double>(pos) / fs < t_end) {
            std::size_t blk_end = pos + block;
            while (next_t >= 0.0 && static_cast<std::size_t>(next_t * fs) < blk_end) {
                int offset = static_cast<int>(static_cast<std::size_t>(next_t * fs) - pos);
                double mu = mu0 * v0 / v;
                double v_reb = voice.impact(v, offset, mu);
                ++bounces;
                double t_hit = next_t;
                if (v_reb < v_min || bounces >= 30) {
                    next_t = -1.0;
                    t_end = t_hit + tail;
                    break;
                }
                next_t = t_hit + voice.last_contact_time() + 2.0 * v_reb / kG;
                if (next_t > t_max) { t_end = t_hit + tail; next_t = -1.0; break; }
                v = v_reb;
            }
            voice.process_stems(obuf, fbuf, block);
            ob.insert(ob.end(), obuf, obuf + block);
            fl.insert(fl.end(), fbuf, fbuf + block);
            pos = blk_end;
        }
        RenderedScene sc;
        sc.name = "drop_" + ball.slug + "_from-1m_onto_wood-floor";
        sc.object_stem = ob;
        sc.floor_stem = fl;
        sc.mix.resize(ob.size());
        for (std::size_t i = 0; i < ob.size(); ++i) sc.mix[i] = ob[i] + fl[i];
        sc.facts.push_back(std::to_string(bounces) + " bounces, calibrated mu " + fmt("%.4f", mu0) +
                           " s/m (restitution 0.8 at " + fmt("%.2f", v0) + " m/s)");
        out.push_back(std::move(sc));
    }
    return out;
}

// ------------------------------------------------------------ output

static void write_le(std::ofstream& f, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) f.put(static_cast<char>((v >> (8 * i)) & 0xff));
}

void write_wav16(const std::string& path, const std::vector<double>& y, double fs) {
    std::ofstream f(path, std::ios::binary);
    uint32_t n = static_cast<uint32_t>(y.size()), rate = static_cast<uint32_t>(fs);
    f.write("RIFF", 4); write_le(f, 36 + n * 2, 4); f.write("WAVEfmt ", 8);
    write_le(f, 16, 4); write_le(f, 1, 2); write_le(f, 1, 2); write_le(f, rate, 4);
    write_le(f, rate * 2, 4); write_le(f, 2, 2); write_le(f, 16, 2);
    f.write("data", 4); write_le(f, n * 2, 4);
    for (double v : y) {
        double c = std::clamp(v, -1.0, 1.0);
        auto s = static_cast<int16_t>(std::lround(c * 32767.0));
        write_le(f, static_cast<uint16_t>(s), 2);
    }
}

void write_wav_float(const std::string& path, const std::vector<double>& y, double fs) {
    std::ofstream f(path, std::ios::binary);
    uint32_t n = static_cast<uint32_t>(y.size()), rate = static_cast<uint32_t>(fs);
    f.write("RIFF", 4); write_le(f, 36 + n * 4, 4); f.write("WAVEfmt ", 8);
    write_le(f, 16, 4); write_le(f, 3, 2); write_le(f, 1, 2); write_le(f, rate, 4);
    write_le(f, rate * 4, 4); write_le(f, 4, 2); write_le(f, 32, 2);
    f.write("data", 4); write_le(f, n * 4, 4);
    for (double v : y) {
        float s = static_cast<float>(v);
        f.write(reinterpret_cast<const char*>(&s), 4);
    }
}

void write_family(const std::string& dir, const std::string& title,
                  const std::vector<RenderedScene>& scenes, double fs, double peak_db) {
    namespace fsys = std::filesystem;
    fsys::create_directories(dir);
    for (const auto& e : fsys::directory_iterator(dir))   // stale renders
        if (e.path().extension() == ".wav") fsys::remove(e.path());
    double peak = 1e-30;
    for (const auto& s : scenes)
        for (double v : s.mix) peak = std::max(peak, std::abs(v));
    // shared gain from the UNTRIMMED mixes, so a trim really lowers a file
    double g = std::pow(10.0, peak_db / 20.0) / peak;
    std::ostringstream md;
    md << "# " << title << "\n\nRendered by the C++ core (cpp/, procaudio_render). "
       << "All files share one gain: louder here = louder in the physics, except "
       << "where a level trim is listed. Family peak " << peak
       << " Pa (proxy at 1 m) -> " << peak_db << " dBFS.\n\n";
    std::printf("  family peak %.4g Pa (1 m pressure proxy); rms per file:", peak);
    for (const auto& s : scenes) {
        double ss = 0.0;
        for (double v : s.mix) ss += v * v;
        std::printf(" %.3g", std::sqrt(ss / static_cast<double>(s.mix.size())));
    }
    std::printf(" Pa\n");
    for (const auto& s : scenes) {
        std::vector<double> y = s.mix;
        double gs = g * std::pow(10.0, s.trim_db / 20.0);
        for (auto& v : y) v *= gs;
        apply_fade(y, fs);
        write_wav16((fsys::path(dir) / (s.name + ".wav")).string(), y, fs);
        md << "## `" << s.name << ".wav`\n\n";
        for (const auto& f : s.facts) md << "- " << f << "\n";
        if (s.trim_db != 0.0)
            md << "- level trim " << s.trim_db << " dB (designer setting, not physics)\n";
        md << "\n";
    }
    std::ofstream(fsys::path(dir) / "README.md") << md.str();
}

}  // namespace pa
