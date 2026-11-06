// Offline scenes that mirror the Python listening sets, for A/B against
// proto/renders/scenes/: roll_spheres (scenes/roll.py spheres), drag_boxes
// (scenes/drag.py boxes), ball_drop (scenes/ball_drop.py, one floor).
#pragma once

#include <string>
#include <vector>

namespace pa {

struct RenderedScene {
    std::string name;
    std::vector<double> mix, object_stem, floor_stem;   // Pa proxy
    double trim_db = 0.0;                               // designer trim (<= 0)
    std::vector<std::string> facts;
};

std::vector<RenderedScene> render_roll_spheres(double fs);
// speed: optional pull-speed track (m/s at fs) replacing the built-in one
// (golden tests feed the Python track so both sides drag identically)
std::vector<RenderedScene> render_drag_boxes(double fs,
                                             const std::vector<double>* speed = nullptr);
std::vector<RenderedScene> render_ball_drop(double fs);

// One shared gain per family (loudest peak -> peak_db dBFS), trims applied
// after it; writes <dir>/<name>.wav (16-bit PCM mono) and a README.md.
void write_family(const std::string& dir, const std::string& title,
                  const std::vector<RenderedScene>& scenes, double fs,
                  double peak_db = -3.0);

void write_wav16(const std::string& path, const std::vector<double>& y, double fs);
void write_wav_float(const std::string& path, const std::vector<double>& y, double fs);

}  // namespace pa
