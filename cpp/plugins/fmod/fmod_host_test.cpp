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

enum { P_OBJECT, P_SURFACE, P_MOTION, P_SPEED, P_IMPACT, P_LEVEL };
enum { M_IMPACT, M_DROP, M_ROLL, M_SLIDE };

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
    auto render = [&](const char* label, double seconds) -> int {
        double t0 = t;
        int n = static_cast<int>(seconds * 48000.0 / block);
        for (int i = 0; i < n; ++i) {
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
    auto play = [&](int obj, int surf, int motion, float speed, float impact, double* ms) -> int {
        if (ch) {   // the update renders one more block: count it, label it
            ch->stop();
            if (render("gap", static_cast<double>(block) / 48000.0)) return 1;
            dsp->release();
        }
        auto t0 = std::chrono::steady_clock::now();
        CHECK(sys->createDSPByPlugin(handle, &dsp));
        CHECK(dsp->setParameterInt(P_OBJECT, obj));
        CHECK(dsp->setParameterInt(P_SURFACE, surf));
        CHECK(dsp->setParameterInt(P_MOTION, motion));
        CHECK(dsp->setParameterFloat(P_SPEED, speed));
        CHECK(dsp->setParameterFloat(P_IMPACT, impact));
        CHECK(dsp->setParameterFloat(P_LEVEL, motion == 2 ? 35.0f : motion == 3 ? 50.0f : 0.0f));
        CHECK(sys->playDSP(dsp, nullptr, false, &ch));
        *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return 0;
    };
    // let the background prewarm finish (Studio does this at start-up)
    std::this_thread::sleep_for(std::chrono::seconds(12));
    std::printf("plug-in loaded %.1f s ago (prewarm running in the background)\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t_load).count());

    double ms = 0.0;
    if (play(3, 0, M_IMPACT, 0.0f, 2.0f, &ms)) return 1;      // steel ball L, one hit
    std::printf("  Play steel ball L / Impact: instance ready in %.1f ms\n", ms);
    if (render("impact", 1.5)) return 1;
    CHECK(dsp->setParameterFloat(P_IMPACT, 1.5f));            // new value: fires again
    if (render("impact_retrigger", 1.5)) return 1;
    if (play(4, 2, M_DROP, 0.0f, 2.5f, &ms)) return 1;        // card box S dropped on concrete
    std::printf("  Play card box S / Drop: instance ready in %.1f ms\n", ms);
    if (render("drop_box", 2.0)) return 1;
    if (play(3, 0, M_DROP, 0.0f, 2.0f, &ms)) return 1;        // steel ball L dropped
    if (render("drop_ball", 2.0)) return 1;
    if (play(3, 0, M_ROLL, 0.5f, 2.0f, &ms)) return 1;        // roll: Impact must NOT fire
    std::printf("  Play steel ball L / Roll: instance ready in %.1f ms\n", ms);
    if (render("roll", 2.5)) return 1;
    if (play(6, 2, M_SLIDE, 0.5f, 2.0f, &ms)) return 1;       // steel box S slides on concrete
    std::printf("  Play steel box S / Slide: instance ready in %.1f ms\n", ms);
    if (render("slide", 2.5)) return 1;
    CHECK(dsp->setParameterFloat(P_SPEED, 0.0f));
    if (render("stopped", 4.0)) return 1;

    // teardown: stop, let the mixer drop the DSP, release it, then the system
    CHECK(ch->stop());
    CHECK(sys->update());
    CHECK(dsp->release());
    CHECK(sys->release());
    std::printf("rendered %.1f s through FMOD (block %u) -> %s\n", t, block, argv[2]);
    return 0;
}
