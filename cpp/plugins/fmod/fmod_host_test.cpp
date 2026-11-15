// Integration test: load procaudio_fmod.dll into the REAL FMOD engine (Core
// API), drive it the way FMOD Studio does (a fresh instance per Play,
// parameters set right after creation) and render through FMOD's
// non-real-time WAV writer.
//
//   procaudio_fmod_host <procaudio_fmod.dll> <out.wav> <segments.txt>
//
// segments.txt lists "label start_s end_s" per segment, checked by
// cpp/tests/fmod_host_check.py.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "fmod.hpp"
#include "fmod_errors.h"

#define CHECK(x)                                                                   \
    do {                                                                           \
        FMOD_RESULT r_ = (x);                                                      \
        if (r_ != FMOD_OK) {                                                       \
            std::printf("FMOD error %d (%s) at %s\n", r_, FMOD_ErrorString(r_), #x); \
            return 1;                                                              \
        }                                                                          \
    } while (0)

// plug-in v2.4 parameter indices (procaudio_fmod.cpp)
enum {
    P_O_SHAPE, P_O_MATERIAL, P_O_MOUNT, P_O_SIZE, P_O_THICK, P_O_HIT,
    P_T_SHAPE, P_T_MATERIAL, P_T_FINISH, P_T_MOUNT, P_T_SIZE, P_T_THICK, P_T_HIT,
    P_MOTION, P_SPEED, P_RENDER, P_LEVEL, P_CONTACTS, P_FIT
};
enum { M_IMPACT, M_DROP, M_ROLL, M_SLIDE };
enum { BALL, STICK, BOX, PANEL, GROUND };
// the Target dropdown: 0 = Same as object, then the shapes
inline int target(int shape) { return shape + 1; }
constexpr int SAME_AS_OBJECT = 0;
enum { METAL, GLASS, WOOD, PLASTIC, RUBBER, STONE, CARDBOARD, CONCRETE, CERAMIC };
enum { BARE, POLISHED, TILED, CARPETED };
enum { FREE, HELD, RESTING, BUILTIN };

// A Play's settings: only values that differ from the deck defaults are
// set, the way a Studio event stores its plug-in instrument.
struct Settings {
    std::vector<std::pair<int, int>> ints;
    std::vector<std::pair<int, float>> floats;
};

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: procaudio_fmod_host <plugin.dll> <out.wav> <segments.txt>\n");
        return 2;
    }
    FMOD::System* sys = nullptr;
    CHECK(FMOD::System_Create(&sys));
    CHECK(sys->setOutput(FMOD_OUTPUTTYPE_WAVWRITER_NRT));
    CHECK(sys->setSoftwareFormat(48000, FMOD_SPEAKERMODE_MONO, 0));
    CHECK(sys->init(32, FMOD_INIT_NORMAL, argv[2]));   // extradriverdata = file name

    unsigned int handle = 0;
    auto t_load = std::chrono::steady_clock::now();
    CHECK(sys->loadPlugin(argv[1], &handle));   // registers it: the cache prewarm starts
    unsigned int block = 0;
    int nbuf = 0;
    CHECK(sys->getDSPBufferSize(&block, &nbuf));
    std::ofstream seg(argv[3]);
    double t = 0.0;
    // realtime = true paces the mix blocks to the wall clock, as a live
    // Studio Play does: a voice built on the plug-in's own thread then arrives
    // after the same number of blocks as in Studio (NRT alone runs ~50x faster)
    auto render = [&](const char* label, double seconds, bool realtime = false) -> int {
        double t0 = t;
        int n = static_cast<int>(seconds * 48000.0 / block);
        auto wall0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) {
            if (realtime)
                std::this_thread::sleep_until(wall0 + std::chrono::microseconds(
                    static_cast<long long>(1e6 * i * block / 48000.0)));
            FMOD_RESULT r = sys->update();   // NRT: one update = one mix block
            if (r != FMOD_OK) { std::printf("update: %s\n", FMOD_ErrorString(r)); return 1; }
        }
        t += n * static_cast<double>(block) / 48000.0;
        seg << label << " " << t0 << " " << t << "\n";
        return 0;
    };
    // an FMOD Studio "Play": new instance, parameters, play
    FMOD::DSP* dsp = nullptr;
    FMOD::Channel* ch = nullptr;
    // Level presets of procaudio_fmod.plugin.js: Impact/Drop 0, Roll +35, Slide +50 dB
    auto play = [&](const Settings& s, int motion, float speed, float impact, double* ms) -> int {
        if (ch) {   // the update renders one more block: count it, label it
            ch->stop();
            if (render("gap", static_cast<double>(block) / 48000.0)) return 1;
            dsp->release();
        }
        auto t0 = std::chrono::steady_clock::now();
        CHECK(sys->createDSPByPlugin(handle, &dsp));
        for (const auto& p : s.ints) CHECK(dsp->setParameterInt(p.first, p.second));
        for (const auto& p : s.floats) CHECK(dsp->setParameterFloat(p.first, p.second));
        CHECK(dsp->setParameterInt(P_MOTION, motion));
        // one Speed knob: the hit / landing speed for Impact and Drop, the
        // rolling / sliding speed for Roll and Slide
        CHECK(dsp->setParameterFloat(P_SPEED, motion <= M_DROP ? impact : speed));
        CHECK(dsp->setParameterFloat(P_LEVEL, motion == 2 ? 35.0f : motion == 3 ? 50.0f : 0.0f));
        CHECK(sys->playDSP(dsp, nullptr, false, &ch));
        *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return 0;
    };
    // let the background prewarm finish (Studio does this at start-up)
    std::this_thread::sleep_for(std::chrono::seconds(20));   // prewarm: ~18 s
    std::printf("plug-in loaded %.1f s ago (prewarm running in the background)\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t_load).count());

    const Settings deflt;                                       // steel ball 6 cm, varnished wood floor
    const Settings concrete{{{P_T_SHAPE, target(GROUND)}, {P_T_MATERIAL, CONCRETE}, {P_T_FINISH, BARE}},
                            {{P_T_SIZE, 4.0f}}};
    Settings card_box = concrete, steel_box = concrete;
    card_box.ints.push_back({P_O_SHAPE, BOX});
    card_box.ints.push_back({P_O_MATERIAL, CARDBOARD});
    card_box.floats.push_back({P_O_SIZE, 0.30f});
    card_box.floats.push_back({P_O_THICK, 0.004f});
    steel_box.ints.push_back({P_O_SHAPE, BOX});
    steel_box.floats.push_back({P_O_SIZE, 0.30f});
    steel_box.floats.push_back({P_O_THICK, 0.0008f});
    // a held wooden stick (60 cm) knocks a free wooden ball (12 cm): no preset
    const Settings stick_ball{{{P_O_SHAPE, STICK}, {P_O_MATERIAL, WOOD}, {P_O_MOUNT, HELD},
                               {P_T_SHAPE, target(BALL)}, {P_T_MATERIAL, WOOD}, {P_T_FINISH, BARE},
                               {P_T_MOUNT, FREE}},
                              {{P_O_SIZE, 0.6f}, {P_O_HIT, 1.0f}, {P_T_SIZE, 0.12f}}};
    // two equal glass marbles
    const Settings marbles{{{P_O_MATERIAL, GLASS}, {P_T_SHAPE, SAME_AS_OBJECT}}, {{P_O_SIZE, 0.025f}}};
    Settings floor_only = concrete;
    floor_only.ints.push_back({P_RENDER, 2});                  // Target only

    double ms = 0.0;
    if (play(deflt, M_IMPACT, 0.0f, 2.0f, &ms)) return 1;     // steel ball, one hit
    std::printf("  Play steel ball / Impact: instance ready in %.1f ms\n", ms);
    if (render("impact", 1.5)) return 1;
    CHECK(dsp->setParameterFloat(P_SPEED, 1.5f));             // new value: fires again
    if (render("impact_retrigger", 1.5)) return 1;
    if (play(card_box, M_DROP, 0.0f, 2.5f, &ms)) return 1;    // card box dropped on concrete
    std::printf("  Play card box / Drop on concrete: instance ready in %.1f ms\n", ms);
    if (render("drop_box", 2.0)) return 1;
    if (play(deflt, M_DROP, 0.0f, 2.0f, &ms)) return 1;       // steel ball dropped
    if (render("drop_ball", 2.0)) return 1;
    if (play(deflt, M_ROLL, 0.5f, 2.0f, &ms)) return 1;       // roll at 0.5 m/s: never a hit
    std::printf("  Play steel ball / Roll: instance ready in %.1f ms\n", ms);
    if (render("roll", 2.5)) return 1;
    if (play(steel_box, M_SLIDE, 0.5f, 2.0f, &ms)) return 1;  // steel box slides on concrete
    std::printf("  Play steel box / Slide: instance ready in %.1f ms\n", ms);
    if (render("slide", 2.5)) return 1;
    CHECK(dsp->setParameterFloat(P_SPEED, 0.0f));
    if (render("stopped", 4.0)) return 1;
    // new in v2.0
    if (play(stick_ball, M_IMPACT, 0.0f, 3.0f, &ms)) return 1;   // not prewarmed: a cold pair
    std::printf("  Play held stick -> wooden ball (cold pair): instance ready in %.1f ms\n", ms);
    {   // the dials' own value labels (what Studio can show next to a dial)
        const struct { int p; const char* name; } labels[] = {
            {P_O_SIZE, "Obj Size"}, {P_O_THICK, "Obj Thickness"}, {P_O_HIT, "Obj Hit Pos"},
            {P_T_SIZE, "Tgt Size"}, {P_T_THICK, "Tgt Thickness"}, {P_T_HIT, "Tgt Hit Pos"},
            {P_SPEED, "Speed"}};
        for (const auto& l : labels) {
            float v = 0.0f;
            char str[32] = "";
            CHECK(dsp->getParameterFloat(l.p, &v, str, sizeof(str)));
            std::printf("    label %-13s = %-6.3g -> \"%s\"\n", l.name, v, str);
        }
    }
    if (render("pair_stick_ball", 1.5, true)) return 1;
    if (play(marbles, M_IMPACT, 0.0f, 1.0f, &ms)) return 1;
    std::printf("  Play glass marble -> same marble (cold pair): instance ready in %.1f ms\n", ms);
    {   // with Target = Same as object the Tgt dials are ignored: they say so
        float v = 0.0f;
        char str[32] = "";
        CHECK(dsp->getParameterFloat(P_T_SIZE, &v, str, sizeof(str)));
        std::printf("    label Tgt Size (same as object) -> \"%s\"\n", str);
    }
    if (render("pair_same", 1.0, true)) return 1;
    if (play(floor_only, M_IMPACT, 0.0f, 2.0f, &ms)) return 1;    // steel ball on concrete, slab only
    if (render("render_target", 1.0)) return 1;
    if (play(deflt, M_IMPACT, 0.0f, 2.0f, &ms)) return 1;      // the reference ring
    if (render("ring_ref", 1.5)) return 1;
    if (play(deflt, M_IMPACT, 0.0f, 2.0f, &ms)) return 1;
    if (render("size_swap", 0.3)) return 1;                       // ringing ...
    CHECK(dsp->setParameterFloat(P_O_SIZE, 0.031f));              // ... while the body changes
    if (render("size_swap_after", 1.2)) return 1;
    // v2.3: a held ball asked to Roll slides instead, and the Speed label says why
    const Settings held_ball{{{P_O_MOUNT, HELD}}, {}};
    if (play(held_ball, M_ROLL, 0.5f, 0.0f, &ms)) return 1;
    // it slides, so the Slide preset's Level applies (play() set Roll's +35).
    // A smooth ball on a varnished floor is very quiet since the slide
    // surface fix (design doc Part 12): no more groove and grit clicks.
    CHECK(dsp->setParameterFloat(P_LEVEL, 50.0f));
    {
        float v = 0.0f;
        char str[32] = "";
        CHECK(dsp->getParameterFloat(P_SPEED, &v, str, sizeof(str)));
        std::printf("    label Speed (held ball, Roll) -> \"%s\"\n", str);
    }
    if (render("held_roll", 2.0, true)) return 1;   // real time: a cold pair (held ball)
    // v2.4: Fit Length. The steel-ball hit (as ring_ref) fitted to 0.15 s:
    // unchanged at first, then a fade to true silence 1 ms before 0.15 s
    Settings fit_hit = deflt;
    fit_hit.floats.push_back({P_FIT, 0.15f});
    if (play(fit_hit, M_IMPACT, 0.0f, 2.0f, &ms)) return 1;
    {
        float v = 0.0f;
        char str[32] = "";
        CHECK(dsp->getParameterFloat(P_FIT, &v, str, sizeof(str)));
        std::printf("    label Fit Length = %.3g -> \"%s\"\n", v, str);
    }
    if (render("fit_hit", 0.6)) return 1;
    {   // the instrument stops itself (silence) once past its Fit Length
        bool playing = true;
        ch->isPlaying(&playing);
        std::printf("    fit_hit: channel %s after 0.6 s\n", playing ? "still playing" : "ended");
    }
    Settings fit_roll = deflt;                                     // a roll fitted to 0.4 s
    fit_roll.floats.push_back({P_FIT, 0.4f});
    if (play(fit_roll, M_ROLL, 0.5f, 0.0f, &ms)) return 1;
    if (render("fit_roll", 1.0)) return 1;
    // v2.3: a long metal ring must not be cut after 3 s (a 1 m free steel
    // stick dropped on the ground rings for several seconds)
    Settings long_ring = concrete;
    long_ring.ints.push_back({P_O_SHAPE, STICK});
    long_ring.ints.push_back({P_O_MATERIAL, METAL});
    long_ring.floats.push_back({P_O_SIZE, 1.0f});
    if (play(long_ring, M_IMPACT, 0.0f, 2.0f, &ms)) return 1;
    if (render("long_ring", 6.0, true)) return 1;   // real time: the voice is a cold pair

    // teardown: stop, let the mixer drop the DSP, release it, then the system
    CHECK(ch->stop());
    CHECK(sys->update());
    CHECK(dsp->release());
    CHECK(sys->release());
    std::printf("rendered %.1f s through FMOD (block %u) -> %s\n", t, block, argv[2]);
    return 0;
}
