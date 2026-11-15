// procaudio_render: offline renderer and golden-test runner for the core.
//
//   procaudio_render scene <roll_spheres|drag_boxes|ball_drop> <out_dir> [fs] [speed.f64]
//     (speed.f64: drag_boxes only, a float64 pull-speed track at fs)
//   procaudio_render golden <modal|impact|roll|slide> <case_dir>
//
// Golden cases are written by cpp/tests/golden.py: <case_dir>/params.txt
// ("key value" lines) plus raw little-endian float64 arrays (*.f64); the
// result goes to <case_dir>/cpp_*.f64 and cpp_stats.txt.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "procaudio/catalog.h"
#include "procaudio/scenes.h"
#include "procaudio/voice.h"

using namespace pa;

static std::vector<double> read_f64(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::streamsize n = f.tellg();
    f.seekg(0);
    std::vector<double> v(static_cast<std::size_t>(n) / sizeof(double));
    f.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(double)));
    return v;
}

static void write_f64(const std::string& path, const std::vector<double>& v) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(double)));
}

static std::map<std::string, std::string> read_params(const std::string& dir) {
    std::map<std::string, std::string> m;
    std::ifstream f(dir + "/params.txt");
    std::string k, v;
    while (f >> k >> v) m[k] = v;
    return m;
}

static double num(const std::map<std::string, std::string>& m, const std::string& k) {
    auto it = m.find(k);
    if (it == m.end()) throw std::runtime_error("missing param " + k);
    return std::stod(it->second);
}

static std::vector<Mode> modes_from(const std::vector<double>& a) {
    std::vector<Mode> m;
    for (std::size_t i = 0; i + 3 < a.size(); i += 4) m.push_back({a[i], a[i + 1], a[i + 2], a[i + 3]});
    return m;
}

static int golden(const std::string& kind, const std::string& dir) {
    auto P = read_params(dir);
    const double fs = num(P, "fs");
    std::ofstream stats(dir + "/cpp_stats.txt");
    stats.precision(17);
    if (kind == "modal") {
        ModalBank bank;
        bank.setup(modes_from(read_f64(dir + "/modes.f64")), fs);
        auto F = read_f64(dir + "/force.f64");
        std::vector<double> y(F.size());
        for (std::size_t i = 0; i < F.size(); ++i) y[i] = bank.process(F[i]);
        write_f64(dir + "/cpp_out.f64", y);
    } else if (kind == "impact") {
        auto r = simulate_impact_implicit(num(P, "m_s"), num(P, "v_in"), num(P, "k"),
                                          num(P, "alpha"), num(P, "lam"),
                                          modes_from(read_f64(dir + "/modes.f64")), fs,
                                          num(P, "t_max"), static_cast<int>(num(P, "oversample")));
        write_f64(dir + "/cpp_force.f64", r.force);
        stats << "contact_time " << r.contact_time << "\nf_max " << r.f_max << "\nv_out "
              << r.v_out << "\nn_contacts " << r.n_contacts << "\n";
    } else if (kind == "roll") {
        RollParams p;
        p.radius = num(P, "radius");
        p.mass = num(P, "mass");
        p.k = num(P, "k");
        p.alpha = num(P, "alpha");
        p.zeta = num(P, "zeta");
        p.oversample = static_cast<int>(num(P, "oversample"));
        RollExciter ex;
        ex.prepare_from_tables(fs, p, read_f64(dir + "/surface.f64"), read_f64(dir + "/rim.f64"),
                               num(P, "rim_dx"));
        auto v = read_f64(dir + "/speed.f64");
        std::vector<double> F(v.size());
        for (std::size_t i = 0; i < v.size(); ++i) F[i] = ex.process(v[i]);
        write_f64(dir + "/cpp_force.f64", F);
        stats << "airborne " << ex.airborne_fraction() << "\nlandings_per_s "
              << ex.landings_per_second() << "\noversample " << ex.oversample() << "\n";
    } else if (kind == "slide") {
        SlideParams p;
        p.N0 = num(P, "N0");
        p.m_p = num(P, "m_p");
        p.slider_mat = material(P.at("slider_mat"));
        p.floor_mat = material(P.at("floor_mat"));
        p.wobble = num(P, "wobble");
        p.oversample = static_cast<int>(num(P, "oversample"));
        int nt = static_cast<int>(num(P, "n_tracks"));
        p.n_tracks = nt;
        std::vector<std::vector<double>> tabs;
        for (int j = 0; j < nt; ++j) tabs.push_back(read_f64(dir + "/tab" + std::to_string(j) + ".f64"));
        SlideExciter ex;
        ex.prepare_from_tables(fs, p, tabs);
        auto v = read_f64(dir + "/velocity.f64");
        std::vector<double> F(v.size());
        for (std::size_t i = 0; i < v.size(); ++i) F[i] = ex.process(v[i]);
        write_f64(dir + "/cpp_force.f64", F);
        stats << "lift_off " << ex.lift_off_fraction() << "\nlandings_per_s "
              << ex.landings_per_second() << "\n";
    } else if (kind == "tile") {
        auto tile = surface_tile(surface_spec(P.at("surface")),
                                 static_cast<std::size_t>(num(P, "n")), num(P, "sigma"),
                                 static_cast<uint64_t>(num(P, "seed")));
        write_f64(dir + "/cpp_tile.f64", tile);
    } else if (kind == "box_sound") {
        // a box body (scenes drag_boxes factory) driven by a given force
        Body box = box_body(num(P, "a"), num(P, "b"), num(P, "c"), P.at("mat"), num(P, "wall"),
                            num(P, "loss"), "test", num(P, "cx"), num(P, "cy"));
        add_contact_damping(box, num(P, "contact_loss"));
        SoundingBody snd;
        snd.setup(box, fs);
        auto F = read_f64(dir + "/force.f64");
        std::vector<double> y(F.size());
        for (std::size_t i = 0; i < F.size(); ++i) y[i] = snd.process(F[i], -1.0);
        write_f64(dir + "/cpp_out.f64", y);
        stats << "n_modes " << box.modes.size() << "\nmass " << box.mass << "\n";
    } else {
        std::cerr << "unknown golden kind " << kind << "\n";
        return 2;
    }
    return 0;
}

// voice build times: cold (nothing cached), warm (cached), full prewarm
static int bench(double fs) {
    using clk = std::chrono::steady_clock;
    auto ms = [](clk::time_point a) {
        return std::chrono::duration<double, std::milli>(clk::now() - a).count();
    };
    for (int o : {3, 6}) {
        auto t0 = clk::now();
        auto v = make_preset_voice(o, 0, fs);
        double cold = ms(t0);
        t0 = clk::now();
        auto w = make_preset_voice(o, 0, fs);
        std::printf("  %-13s on %s: cold %.0f ms, warm %.1f ms\n", object_preset_name(o),
                    surface_preset_name(0), cold, ms(t0));
    }
    {   // the plug-in's start-up prewarm, and one cold rounded slider
        auto t1 = clk::now();
        prewarm_desc_presets(fs);
        std::printf("  desc prewarm (plug-in start-up): %.1f s\n", ms(t1) / 1000.0);
        BodyDesc o, t = preset_surface_desc(0);
        o.shape = Shape::Ball; o.material = "metal"; o.size = 0.06; o.mount = Mount::Held;
        t1 = clk::now();
        auto v = make_desc_voice(o, t, fs, true);
        std::printf("  held steel ball slide voice, cold: %.0f ms\n", ms(t1));
    }
    auto t0 = clk::now();
    prewarm_presets(fs);
    std::printf("  prewarm of all %d x %d presets: %.1f s\n", num_object_presets(),
                num_surface_presets(), ms(t0) / 1000.0);
    return 0;
}

// ---------------------------------------------------------- level survey
// How loud the plug-in is (Level 0, calibration 0 dBFS = 63 Pa at 1 m) for
// many object / target / motion / speed cases. Short-term level = the
// loudest 50 ms RMS window, the same yardstick for hits and for continuous
// motions. One line per case: CSV on stdout.
struct LevelCase { const char* name; BodyDesc o, t; Action motion; double v; };

static BodyDesc desc(Shape s, const char* mat, double size, Mount m, Finish f = Finish::Bare) {
    BodyDesc d;
    d.shape = s; d.material = mat; d.size = size; d.mount = m; d.finish = f;
    return d;
}

static void measure(const LevelCase& c, double fs, double* peak_db, double* st_db) {
    auto v = make_desc_voice(c.o, c.t, fs, c.motion == Action::Roll || c.motion == Action::Slide);
    const double secs = (c.motion == Action::Roll || c.motion == Action::Slide) ? 2.5 : 1.5;
    const int n = static_cast<int>(secs * fs);
    std::vector<double> y(n), ob(512), tb(512);
    if (c.motion == Action::Impact) v->impact(c.v, 0);
    if (c.motion == Action::Drop) v->drop(c.v, 0);
    if (c.motion == Action::Roll) v->set_motion(v->roll() ? Motion::Roll : Motion::Slide);
    if (c.motion == Action::Slide) v->set_motion(Motion::Slide);
    if (c.motion == Action::Roll || c.motion == Action::Slide) v->set_target_speed(c.v);
    for (int i = 0; i < n; i += 512) {
        const int k = std::min(512, n - i);
        v->process_stems(ob.data(), tb.data(), k);
        for (int j = 0; j < k; ++j) y[i + j] = (ob[j] + tb[j]) / 63.0;
    }
    double pk = 0.0, st = 0.0;
    const int w = static_cast<int>(0.05 * fs);
    // continuous motions: the steady part (after 0.5 s); hits: everything
    const int from = (c.motion == Action::Roll || c.motion == Action::Slide) ? static_cast<int>(0.5 * fs) : 0;
    double acc = 0.0;
    for (int i = from; i < n; ++i) {
        pk = std::max(pk, std::abs(y[i]));
        acc += y[i] * y[i];
        if (i - from >= w) acc -= y[i - w] * y[i - w];
        if (i - from >= w - 1) st = std::max(st, acc / w);
    }
    *peak_db = 20.0 * std::log10(pk + 1e-15);
    *st_db = 10.0 * std::log10(st + 1e-30);
}

static int levels(double fs) {
    const Shape BALL = Shape::Ball, STICK = Shape::Stick, BOX = Shape::Box, PANEL = Shape::Panel,
                GROUND = Shape::Ground;
    const Mount FREE = Mount::Free, HELD = Mount::Held, BUILT = Mount::BuiltIn;
    struct Tg { const char* name; BodyDesc d; };
    const Tg targets[] = {
        {"concrete ground", desc(GROUND, "concrete", 4.0, BUILT)},
        {"metal ground", desc(GROUND, "metal", 4.0, BUILT)},
        {"polished wood floor", desc(PANEL, "wood", 2.4, BUILT, Finish::Polished)},
        {"carpeted wood floor", desc(PANEL, "wood", 2.4, BUILT, Finish::Carpeted)},
    };
    struct Ob { const char* name; BodyDesc d; };
    const Ob objects[] = {
        {"wood ball 6cm", desc(BALL, "wood", 0.06, FREE)},
        {"metal ball 6cm", desc(BALL, "metal", 0.06, FREE)},
        {"rubber ball 6cm", desc(BALL, "rubber", 0.06, FREE)},
        {"glass ball 2cm", desc(BALL, "glass", 0.02, FREE)},
        {"wood ball 20cm", desc(BALL, "wood", 0.20, FREE)},
        {"plastic ball 20cm", desc(BALL, "plastic", 0.20, FREE)},
        {"held wood stick 60cm", desc(STICK, "wood", 0.60, HELD)},
        {"metal stick 100cm", desc(STICK, "metal", 1.0, FREE)},
        {"cardboard box 30cm", desc(BOX, "cardboard", 0.30, FREE)},
        {"metal box 30cm", desc(BOX, "metal", 0.30, FREE)},
        {"wood box 30cm", desc(BOX, "wood", 0.30, FREE)},
    };
    const struct { Action a; double v; } motions[] = {
        {Action::Impact, 0.5}, {Action::Impact, 2.0}, {Action::Impact, 5.0},
        {Action::Drop, 2.0},
        {Action::Slide, 0.1}, {Action::Slide, 0.3}, {Action::Slide, 1.0},
        {Action::Roll, 0.1}, {Action::Roll, 0.3}, {Action::Roll, 1.0},
    };
    std::printf("object,target,motion,speed,peak_dbfs,short_term_dbfs\n");
    for (const auto& t : targets)
        for (const auto& o : objects)
            for (const auto& m : motions) {
                const ActionCheck chk = check_action(o.d, t.d, m.a);
                if (chk.use != m.a) continue;   // the plug-in would play something else
                LevelCase c{o.name, o.d, t.d, m.a, m.v};
                double pk, st;
                measure(c, fs, &pk, &st);
                std::printf("%s,%s,%s,%.2g,%.1f,%.1f\n", o.name, t.name, action_name(m.a), m.v, pk, st);
                std::fflush(stdout);
            }
    return 0;
}

// ---------------------------------------------------------- probe
// Renders named cases to <dir>/<name>.f32 (mono float32, 0 dBFS = 63 Pa)
// and prints contact facts: for tuning by measurement.
static int probe(const std::string& dir, double fs) {
    const Mount FREE = Mount::Free, BUILT = Mount::BuiltIn, HELD = Mount::Held;
    const BodyDesc concrete = desc(Shape::Ground, "concrete", 4.0, BUILT);
    const BodyDesc woodpol = desc(Shape::Panel, "wood", 2.4, BUILT, Finish::Polished);
    const BodyDesc woodbare = desc(Shape::Panel, "wood", 2.4, BUILT);
    const BodyDesc metalg = desc(Shape::Ground, "metal", 4.0, BUILT);
    const BodyDesc tiled = desc(Shape::Ground, "concrete", 4.0, BUILT, Finish::Tiled);
    const LevelCase cases[] = {
        {"card_box_impact", desc(Shape::Box, "cardboard", 0.30, FREE), concrete, Action::Impact, 2.0},
        {"card_box_drop", desc(Shape::Box, "cardboard", 0.30, FREE), concrete, Action::Drop, 2.0},
        {"card_box_drop_wood", desc(Shape::Box, "cardboard", 0.30, FREE), woodbare, Action::Drop, 2.0},
        {"card_panel_impact", desc(Shape::Panel, "cardboard", 0.50, FREE), concrete, Action::Impact, 2.0},
        {"stick_on_card_box", desc(Shape::Stick, "wood", 0.6, HELD), desc(Shape::Box, "cardboard", 0.30, Mount::Resting), Action::Impact, 2.0},
        {"wood_box_impact", desc(Shape::Box, "wood", 0.30, FREE), concrete, Action::Impact, 2.0},
        {"wood_box_slide_polished", desc(Shape::Box, "wood", 0.30, FREE), woodpol, Action::Slide, 0.3},
        {"wood_box_slide_polished_fast", desc(Shape::Box, "wood", 0.30, FREE), woodpol, Action::Slide, 1.0},
        {"wood_box_slide_barewood", desc(Shape::Box, "wood", 0.30, FREE), woodbare, Action::Slide, 0.3},
        {"wood_box_slide_concrete", desc(Shape::Box, "wood", 0.30, FREE), concrete, Action::Slide, 0.3},
        {"wood_box_slide_metal", desc(Shape::Box, "wood", 0.30, FREE), metalg, Action::Slide, 0.3},
        {"wood_box_slide_tiled", desc(Shape::Box, "wood", 0.30, FREE), tiled, Action::Slide, 0.3},
        {"card_box_slide_polished", desc(Shape::Box, "cardboard", 0.30, FREE), woodpol, Action::Slide, 0.3},
        {"held_ball_slide_polished", desc(Shape::Ball, "wood", 0.06, HELD), woodpol, Action::Slide, 0.3},
    };
    for (const auto& c : cases) {
        const bool cont = c.motion == Action::Roll || c.motion == Action::Slide;
        auto v = make_desc_voice(c.o, c.t, fs, cont);
        const int n = static_cast<int>((cont ? 4.0 : 2.0) * fs);
        std::vector<float> y(n);
        std::vector<double> ob(512), tb(512);
        double e = 0.0;
        if (c.motion == Action::Impact) e = v->impact(c.v, 0) / c.v;
        if (c.motion == Action::Drop) v->drop(c.v, 0);
        if (cont) { v->set_motion(Motion::Slide); v->set_target_speed(c.v); }
        for (int i = 0; i < n; i += 512) {
            const int k = std::min(512, n - i);
            v->process_stems(ob.data(), tb.data(), k);
            for (int j = 0; j < k; ++j) y[i + j] = static_cast<float>((ob[j] + tb[j]) / 63.0);
        }
        std::ofstream f(dir + "/" + c.name + ".f32", std::ios::binary);
        f.write(reinterpret_cast<const char*>(y.data()), static_cast<std::streamsize>(n * sizeof(float)));
        std::printf("%-30s contact %.3f ms, rebound e %.2f, obj mass %.3f kg, k %.3g\n", c.name,
                    1000.0 * v->last_contact_time(), e, v->object().mass,
                    impact_stiffness(v->object(), v->floor()));
    }
    return 0;
}

// ---------------------------------------------------------- motion rules
// Every object (shape x mount) x target (shape x mount) x asked motion:
// what plays (pa::check_action) and why. CSV on stdout; the design doc's
// chart (Part 11) and the panel check (tests/rules_js_check.js) use it.
static int rules() {
    std::printf("obj_shape,obj_mount,tgt_shape,tgt_mount,wanted,plays,reason\n");
    const int ns = static_cast<int>(Shape::Count), nm = static_cast<int>(Mount::Count);
    for (int os = 0; os < ns; ++os)
        for (int om = 0; om < nm; ++om) {
            if (static_cast<Shape>(os) == Shape::Ground && static_cast<Mount>(om) != Mount::BuiltIn)
                continue;   // Ground is always Built-in
            for (int ts = 0; ts < ns; ++ts)
                for (int tm = 0; tm < nm; ++tm) {
                    if (static_cast<Shape>(ts) == Shape::Ground && static_cast<Mount>(tm) != Mount::BuiltIn)
                        continue;
                    BodyDesc o, t;
                    o.shape = static_cast<Shape>(os); o.mount = static_cast<Mount>(om);
                    t.shape = static_cast<Shape>(ts); t.mount = static_cast<Mount>(tm);
                    for (int a = 0; a < 4; ++a) {
                        const ActionCheck c = check_action(o, t, static_cast<Action>(a));
                        std::printf("%s,%s,%s,%s,%s,%s,%s\n", shape_name(o.shape), mount_name(o.mount),
                                    shape_name(t.shape), mount_name(t.mount),
                                    action_name(static_cast<Action>(a)), action_name(c.use), c.reason);
                    }
                }
        }
    return 0;
}

int main(int argc, char** argv) {
    try {
        if (argc >= 2 && std::string(argv[1]) == "rules") return rules();
        if (argc >= 3 && std::string(argv[1]) == "probe")
            return probe(argv[2], argc >= 4 ? std::stod(argv[3]) : 48000.0);
        if (argc >= 2 && std::string(argv[1]) == "levels")
            return levels(argc >= 3 ? std::stod(argv[2]) : 48000.0);
        if (argc >= 2 && std::string(argv[1]) == "bench")
            return bench(argc >= 3 ? std::stod(argv[2]) : 48000.0);
        if (argc >= 4 && std::string(argv[1]) == "golden") return golden(argv[2], argv[3]);
        if (argc >= 4 && std::string(argv[1]) == "scene") {
            std::string fam = argv[2], dir = argv[3];
            double fs = argc >= 5 ? std::stod(argv[4]) : 44100.0;
            auto t0 = std::chrono::steady_clock::now();
            std::vector<RenderedScene> sc;
            std::string title;
            if (fam == "roll_spheres") {
                sc = render_roll_spheres(fs);
                title = "Solid wooden and steel balls rolling on a varnished wooden floor (C++)";
            } else if (fam == "drag_boxes") {
                std::vector<double> sp;
                if (argc >= 6) sp = read_f64(argv[5]);
                sc = render_drag_boxes(fs, argc >= 6 ? &sp : nullptr);
                title = "Cardboard and steel boxes pulled across concrete (C++)";
            } else if (fam == "ball_drop") {
                sc = render_ball_drop(fs);
                title = "Rubber balls dropped from 1 m onto a wooden floor (C++)";
            } else {
                std::cerr << "unknown scene family " << fam << "\n";
                return 2;
            }
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            double audio = 0.0;
            for (const auto& s : sc) audio += s.mix.size() / fs;
            write_family(dir, title, sc, fs);
            for (const auto& s : sc) {
                std::cout << "  " << s.name << "\n";
                for (const auto& f : s.facts) std::cout << "    " << f << "\n";
            }
            std::printf("%s: %zu scenes, %.1f s of audio in %.2f s (incl. table builds) -> %s\n",
                        fam.c_str(), sc.size(), audio, secs, dir.c_str());
            return 0;
        }
        std::cerr << "usage:\n  procaudio_render scene <roll_spheres|drag_boxes|ball_drop> <out_dir> [fs]\n"
                     "  procaudio_render golden <modal|impact|roll|slide> <case_dir>\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
