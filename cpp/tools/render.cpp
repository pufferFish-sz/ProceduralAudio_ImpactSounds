// procaudio_render: offline renderer and golden-test runner for the core.
//
//   procaudio_render scene <roll_spheres|drag_boxes|ball_drop> <out_dir> [fs] [speed.f64]
//     (speed.f64: drag_boxes only, a float64 pull-speed track at fs)
//   procaudio_render golden <modal|impact|roll|slide> <case_dir>
//
// Golden cases are written by cpp/tests/golden.py: <case_dir>/params.txt
// ("key value" lines) plus raw little-endian float64 arrays (*.f64); the
// result goes to <case_dir>/cpp_*.f64 and cpp_stats.txt.
#include <chrono>
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
    auto t0 = clk::now();
    prewarm_presets(fs);
    std::printf("  prewarm of all %d x %d presets: %.1f s\n", num_object_presets(),
                num_surface_presets(), ms(t0) / 1000.0);
    return 0;
}

int main(int argc, char** argv) {
    try {
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
