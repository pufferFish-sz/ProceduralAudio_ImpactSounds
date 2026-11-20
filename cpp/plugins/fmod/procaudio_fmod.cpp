// FMOD sound module (generator: 0 inputs, 1 mono output): any object meeting
// any target (procaudio/body_desc.h, design: proto/docs/plugin_body_design.md).
// Built against the FMOD Engine 2.02.23 SDK (to match FMOD Studio 2.02.23);
// integration-tested in the real FMOD engine by cpp/tests/fmod_host_check.py.
//
// Parameters (v2.4; procaudio_fmod.plugin.js lays them out in Studio's deck):
//   OBJECT  Obj Shape, Obj Material, Obj Mount   int
//           Obj Size (m, log dial), Obj Thickness (m, 0 = auto),
//           Obj Hit Pos (0 center/middle .. 1 edge/end)
//   TARGET  Target  int  Same as object | Ball | Stick | Box | Panel | Ground
//           ("Same as object": two equal bodies; the other Tgt controls
//           are then ignored and their dials say so)
//           Tgt Material, Tgt Finish (Bare | Polished | Tiled | Carpeted:
//           the floor the object hits, lands, rolls or slides on; Panel and
//           Ground only), Tgt Mount, Tgt Size, Tgt Thickness, Tgt Hit Pos
//   Motion  int    Impact: one hit when the instrument starts
//                  Drop:   the object dropped onto the target (balls bounce,
//                          boxes rock on their edges and settle)
//                  Roll / Slide: continuous, no hits (Roll needs a Ball;
//                          other shapes slide)
//   Speed   float  m/s, one knob whose meaning follows Motion: the hit /
//                  landing speed for Impact and Drop (fires when the
//                  instrument starts and on every new value), the rolling /
//                  sliding speed for Roll and Slide (smoothed, 80 ms)
//   Render  int    Both | Object only | Target only (the other body's own
//                  event renders it, so nothing sounds twice)
//   Level   float  dB over the calibration 0 dBFS = 63 Pa at 1 m (130 dB SPL)
//   Contacts data  ContactMsg[] from game code (sample-accurate offsets)
//   Fit Length float s, 0 = off (v2.4, after Contacts so the indices above
//                  stay): fade to true silence this long after the start,
//                  sample by sample, so a short instrument ends without a
//                  cut. ProcAudio > Ending: Fit writes the instrument's length.
//   Game Motion, Game Target, Game Tgt Mat, Game Tgt Finish,
//   Game Tgt Mount, Game Obj Mount  float, -1 = off (v2.5, appended): game
//                  control of the dropdowns. FMOD Studio applies automation and
//                  game parameters to float plug-in parameters only, so a game
//                  cannot set an int dropdown; each of these overrides one
//                  (its value = the dropdown index) and -1 hands it back to the
//                  deck. Not on the deck: automate them from event parameters.
//
// Dials display what they mean: Size "6.0 cm, 882 g, 7.5 kHz" (size used
// after the shape's clamp, mass, lowest mode), Thickness "auto (2.4 cm)",
// Hit Pos "30% to the edge", Speed "drop 2 m/s = from 20 cm".
//
// What makes sense (pa::check_action): a Drop needs a free object over a
// support, a Roll a free ball on a support (a held ball or a box slides
// instead), nothing moves on a free target in mid-air. The plugin plays the
// closest sensible motion and the Speed label says why ("held/resting ball
// slides"). The ProcAudio Studio script (procaudio_studio_menu.js) hides those
// options in its editor dialog.
//
// The sound ends when it is actually quiet (output below -120 dBFS for
// 100 ms), never on a timer, so long metal rings are not cut. In Studio a
// synchronous instrument still stops dead at the end of its trigger
// region: the ProcAudio script's "Set Up Instrument" makes it Async with an
// AHDSR release.
//
// Render capture (FMOD Studio only, or PROCAUDIO_CAPTURE=1): every Play is
// saved as a 32-bit float WAV in %LOCALAPPDATA%/FMOD Studio/ProcAudio
// (PROCAUDIO_CAPTURE_DIR overrides), newest 20 kept; last_render.txt names
// the newest. The script's "Show Last Render" imports it into the project,
// so its waveform shows in Studio's Assets browser.
//
// Changing a body never cuts the sound: the new voice is built off the
// mixer thread and takes over at the next contact, while the old one keeps
// ringing out (a roll/slide crossfades through the 80 ms speed smoothers).
//
// Threading: FMOD calls create/release/setters on the user thread and
// process() on the mixer thread. A voice whose assets are cached
// (catalog.h, prewarmed with the old presets at registration) is built
// synchronously in ~ms; otherwise a per-instance builder thread builds the
// LATEST request (requests coalesce while it is busy). Voices move between
// threads through atomics; the mixer never allocates or frees one.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include <ctime>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h>
#define PA_HAVE_SSE 1
#endif

#include "fmod.h"
#include "fmod_dsp.h"
#include "procaudio/catalog.h"

namespace {

using pa::BodyDesc;

const char* const kMotionNames[] = {"Impact", "Drop", "Roll", "Slide"};
enum MotionMode { M_IMPACT, M_DROP, M_ROLL, M_SLIDE, M_COUNT, M_NONE = M_COUNT };
const char* const kRenderNames[] = {"Both", "Object only", "Target only"};
enum RenderMode { R_BOTH, R_OBJECT, R_TARGET, R_COUNT };
enum SourceMode { S_DECK, S_SAME };   // the target is set on the deck / is the object
constexpr double kFullScalePa = 63.0;  // 0 dBFS = 63 Pa at 1 m (130 dB SPL)

// What game code sends through the "Contacts" data parameter.
struct ContactMsg {
    uint32_t object_id, surface_id;   // reserved: body registry (phase 4)
    uint8_t kind;          // 0 impact, 1 slide, 2 roll, 3 release, 4 drop
    uint8_t pad[3];
    float t_offset_s;      // time inside the next mix block
    float v_normal;        // impact / drop: normal speed (m/s)
    float v_tangent;       // slide: slip speed; roll: |omega x r| (m/s)
    float normal_force;    // N (reserved)
    float eff_mass;        // kg (reserved)
};

// ---------------------------------------------------------- events

struct Event { int kind; float v; int offset; };   // 0 hit, 1 drop

template <class T, unsigned N>
class SpscQueue {   // one producer thread, one consumer thread
public:
    bool push(const T& e) {
        unsigned h = head_.load(std::memory_order_relaxed), n = (h + 1) % N;
        if (n == tail_.load(std::memory_order_acquire)) return false;
        buf_[h] = e;
        head_.store(n, std::memory_order_release);
        return true;
    }
    bool pop(T& e) {
        unsigned t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        e = buf_[t];
        tail_.store((t + 1) % N, std::memory_order_release);
        return true;
    }
private:
    T buf_[N];
    std::atomic<unsigned> head_{0}, tail_{0};
};

// ---------------------------------------------------------- prewarm

struct Prewarm {
    std::mutex mtx;
    std::thread th;
    std::atomic<bool> stop{false};
    int users = 0;
    // A host that exits without deregistering the plug-in (fmodstudiocl
    // -script) would destroy a joinable thread: std::terminate, 0xC0000409.
    // Joining here could deadlock under the loader lock, so let it go.
    ~Prewarm() {
        stop = true;
        if (th.joinable()) th.detach();
    }
};
Prewarm& prewarm() {
    static Prewarm p;
    return p;
}

// ---------------------------------------------------------- parameters

enum Param {
    P_O_SHAPE, P_O_MATERIAL, P_O_MOUNT, P_O_SIZE, P_O_THICK, P_O_HIT,
    P_T_SHAPE, P_T_MATERIAL, P_T_FINISH, P_T_MOUNT, P_T_SIZE, P_T_THICK, P_T_HIT,
    P_MOTION, P_SPEED, P_RENDER, P_LEVEL, P_CONTACTS,
    P_FIT,      // v2.4, appended: the indices above stay what game code uses
    // v2.5, appended: game control. FMOD Studio applies automation (game
    // parameters) to FLOAT plug-in parameters only; the dropdowns above are
    // ints, so a game could never set them. Each of these floats overrides one
    // dropdown: -1 = off (the deck's value), 0.. = that dropdown's index.
    P_G_MOTION, P_G_TARGET, P_G_T_MATERIAL, P_G_T_FINISH, P_G_T_MOUNT, P_G_O_MOUNT,
    P_COUNT
};
constexpr int P_GAME_FIRST = P_G_MOTION, N_GAME = P_COUNT - P_G_MOTION;
// the dropdown each game parameter overrides
constexpr int kGameTarget[N_GAME] = {P_MOTION, P_T_SHAPE, P_T_MATERIAL, P_T_FINISH, P_T_MOUNT, P_O_MOUNT};

// ---------------------------------------------------------- Fit Length
// FMOD gives a plug-in no way to know its instrument's length, and Studio
// cuts a synchronous instrument dead at its end. Studio's own envelopes
// (the timeline fade-out, an AHDSR release) move the volume once per mix
// block with a curve that stays loud until late, so on a short instrument
// they still end in a cut (-33 .. -41 dBFS for a 0.15 s steel-ball hit).
// The ProcAudio menu writes the instrument's length here instead; the
// plug-in then fades its own output, sample by sample, to true silence just
// before the end. 0 = off: the sound rings as long as it rings.
constexpr double kFitMarginS = 0.001;    // silent this long before the end
double fit_fade_s(double len) { return std::clamp(0.3 * len, 0.005, 0.1); }
// the gain at t seconds after the Play started: 1, then a raised-cosine
// fade to 0 ending kFitMarginS before len, then 0
double fit_gain(double t, double len) {
    const double end = len - kFitMarginS, f = fit_fade_s(len);
    if (t >= end) return 0.0;
    if (t <= end - f) return 1.0;
    return 0.5 - 0.5 * std::cos(3.14159265358979323846 * (end - t) / f);
}

// log-feeling dials (piecewise-linear mapping, evenly spaced points)
float g_size_vals[] = {0.005f, 0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 20.0f};
float g_size_pos[] = {0.0f, 1 / 11.f, 2 / 11.f, 3 / 11.f, 4 / 11.f, 5 / 11.f, 6 / 11.f, 7 / 11.f,
                      8 / 11.f, 9 / 11.f, 10 / 11.f, 1.0f};
float g_thick_vals[] = {0.0f, 0.0005f, 0.001f, 0.002f, 0.005f, 0.01f, 0.02f, 0.05f, 0.1f, 0.3f};
float g_speed_vals[] = {0.0f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f};
float g_speed_pos[] = {0.0f, 1 / 8.f, 2 / 8.f, 3 / 8.f, 4 / 8.f, 5 / 8.f, 6 / 8.f, 7 / 8.f, 1.0f};
float g_fit_vals[] = {0.0f, 0.05f, 0.1f, 0.2f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 30.0f};
float g_fit_pos[] = {0.0f, 1 / 9.f, 2 / 9.f, 3 / 9.f, 4 / 9.f, 5 / 9.f, 6 / 9.f, 7 / 9.f, 8 / 9.f, 1.0f};
float g_thick_pos[] = {0.0f, 1 / 9.f, 2 / 9.f, 3 / 9.f, 4 / 9.f, 5 / 9.f, 6 / 9.f, 7 / 9.f, 8 / 9.f, 1.0f};

const char* g_shape_names[8];
const char* g_target_names[8];   // "Same as object", then the shapes
const char* g_material_names[16];
const char* g_finish_names[8];
const char* g_mount_names[8];

// deck defaults = v1.2 defaults: Steel ball L on the varnished wood floor
BodyDesc default_object() { return pa::preset_object_desc(3); }
BodyDesc default_target() { return pa::preset_surface_desc(0); }

int material_index(const std::string& m) {
    for (int i = 0; i < pa::num_body_materials(); ++i)
        if (m == pa::body_material(i)) return i;
    return 0;
}

// "6.0 cm, 882 g, 7.5 kHz": size used after the clamp, mass, lowest mode
void describe(const BodyDesc& d, char* out, std::size_t n) {
    pa::Body b = pa::make_body(d);
    double s = std::min(pa::size_max(d.shape), std::max(pa::size_min(d.shape), d.size));
    char sz[16], ms[16], fr[16];
    if (s < 0.1) std::snprintf(sz, sizeof(sz), "%.1f cm", s * 100);
    else if (s < 1.0) std::snprintf(sz, sizeof(sz), "%.0f cm", s * 100);
    else std::snprintf(sz, sizeof(sz), "%.2f m", s);
    if (b.mass < 1.0) std::snprintf(ms, sizeof(ms), "%.0f g", b.mass * 1e3);
    else std::snprintf(ms, sizeof(ms), "%.1f kg", b.mass);
    double f = 0.0;
    for (const auto& m : b.modes) f = f == 0.0 ? m.freq : std::min(f, m.freq);
    if (f <= 0.0) std::snprintf(fr, sizeof(fr), "no ring");
    else if (f < 1000.0) std::snprintf(fr, sizeof(fr), "%.0f Hz", f);
    else std::snprintf(fr, sizeof(fr), "%.1f kHz", f / 1e3);
    std::snprintf(out, n, "%s, %s, %s", sz, ms, fr);
}

void describe_thickness(const BodyDesc& d, char* out, std::size_t n) {
    if (d.shape == pa::Shape::Ball) { std::snprintf(out, n, "solid"); return; }
    double s = std::min(pa::size_max(d.shape), std::max(pa::size_min(d.shape), d.size));
    double h = d.thickness > 0.0 ? d.thickness : pa::default_thickness(d.shape, d.material, s);
    char v[16];
    if (h < 0.01) std::snprintf(v, sizeof(v), "%.1f mm", h * 1e3);
    else std::snprintf(v, sizeof(v), "%.1f cm", h * 1e2);
    if (d.thickness > 0.0) std::snprintf(out, n, "%s", v);
    else std::snprintf(out, n, "auto (%s)", v);
}

// "middle" .. "end" for sticks, "center" .. "edge" for plates and boxes
void describe_hit(const BodyDesc& d, char* out, std::size_t n) {
    if (d.shape == pa::Shape::Ball) { std::snprintf(out, n, "any spot (ball)"); return; }
    const bool stick = d.shape == pa::Shape::Stick;
    const int pct = static_cast<int>(std::lround(100.0 * std::min(1.0, std::max(0.0, d.hit))));
    const char* lo = stick ? "middle" : "center";
    const char* hi = stick ? (d.mount == pa::Mount::Held ? "tip" : "end") : "edge";
    if (pct == 0) std::snprintf(out, n, "%s", lo);
    else if (pct == 100) std::snprintf(out, n, "%s", hi);
    else std::snprintf(out, n, "%d%% to the %s", pct, hi);
}

// ---------------------------------------------------------- render capture

// True inside FMOD Studio (the authoring tool), or when PROCAUDIO_CAPTURE=1:
// games never write files.
bool capture_enabled() {
    if (const char* e = std::getenv("PROCAUDIO_CAPTURE")) return e[0] == '1';
#ifdef _WIN32
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring exe(path);
    for (auto& c : exe) c = static_cast<wchar_t>(towlower(c));
    return exe.find(L"fmod studio") != std::wstring::npos ||
           exe.find(L"fmodstudio") != std::wstring::npos;
#else
    return false;
#endif
}

std::filesystem::path capture_dir() {
    if (const char* d = std::getenv("PROCAUDIO_CAPTURE_DIR")) return std::filesystem::path(d);
    const char* la = std::getenv("LOCALAPPDATA");
    return std::filesystem::path(la ? la : ".") / "FMOD Studio" / "ProcAudio";
}

void write_wav_f32(const std::filesystem::path& file, const float* x, std::size_t n, int fs) {
    std::ofstream f(file, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = static_cast<uint32_t>(n * 4);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(3); u16(1); u32(static_cast<uint32_t>(fs));
    u32(static_cast<uint32_t>(fs) * 4); u16(4); u16(32);   // IEEE float, mono
    f.write("data", 4); u32(bytes);
    f.write(reinterpret_cast<const char*>(x), static_cast<std::streamsize>(bytes));
}

std::string slug_of(const BodyDesc& d) {
    char b[64];
    double sz = std::min(pa::size_max(d.shape), std::max(pa::size_min(d.shape), d.size));
    std::snprintf(b, sizeof(b), "%s-%s-%.0fcm", pa::shape_name(d.shape), d.material.c_str(),
                  sz * 100.0);
    return b;
}

// ---------------------------------------------------------- instance state

struct Slot {                      // a built voice and the request it answers
    std::unique_ptr<pa::ContactVoice> v;
    int gen = 0;
};

struct State;

class Builder {   // builds the latest request on its own thread
public:
    ~Builder() { shutdown(); }
    void post(State* s, const BodyDesc& o, const BodyDesc& t, double fs, bool motion, int gen) {
        {
            std::lock_guard<std::mutex> lock(m_);
            req_ = {o, t, fs, motion, gen};
            has_ = true;
            if (!th_.joinable()) th_ = std::thread([this, s] { run(s); });
        }
        cv_.notify_one();
    }
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(m_);
            stop_ = true;
        }
        cv_.notify_one();
        if (th_.joinable()) th_.join();
    }
private:
    struct Req { BodyDesc o, t; double fs; bool motion; int gen; };
    void run(State* s);
    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_;
    bool has_ = false, stop_ = false;
    Req req_;
};

struct State {
    double fs = 48000.0;
    int deck[P_COUNT] = {};        // dropdown values as the deck set them
    float game[N_GAME] = {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};   // overrides, -1 = off
    // user thread
    BodyDesc obj = default_object(), tgt = default_target();
    int source = S_DECK;
    bool built_motion = false;         // the latest voice has roll / slide tables
    int wanted_motion = M_IMPACT;      // what the deck asks for (Motion)
    char motion_reason[32] = "";       // why the played motion differs (or "")
    std::mutex info_mtx;               // dial labels, rebuilt lazily when read
    bool info_dirty = true;
    char obj_size[32] = "", tgt_size[32] = "", obj_thick[32] = "", tgt_thick[32] = "";
    char obj_hit[32] = "", tgt_hit[32] = "";
    // shared
    std::atomic<int> gen{0};
    std::atomic<int> motion{M_IMPACT}, render{R_BOTH};   // motion = what PLAYS (check_action)
    std::atomic<float> speed{2.0f}, level_db{0.0f};
    std::atomic<float> fit_len{0.0f};        // Fit Length (s), 0 = off
    std::atomic<long long> play_pos{0};      // samples since this Play started
    std::atomic<bool> start_pending{true};   // fire Impact / Drop on first process
    std::atomic<Slot*> pending{nullptr};
    SpscQueue<Slot*, 16> retired;            // mixer -> user thread
    SpscQueue<Event, 256> events;            // user -> mixer thread
    // mixer thread
    Slot* active = nullptr;
    Slot* fading = nullptr;
    unsigned fade_quiet = 0, fade_age = 0;
    unsigned quiet_samples = 0;
    // render capture (Studio): the mixer appends, release() writes the WAV
    std::vector<float> cap;
    std::size_t cap_n = 0;
    Builder builder;

    BodyDesc target() const { return source == S_SAME ? obj : tgt; }

    // user or builder thread: hand a voice to the mixer if it is still wanted
    std::mutex install_mtx;            // the gen check and the hand-over are one step
    void install(std::unique_ptr<pa::ContactVoice> v, int g) {
        std::lock_guard<std::mutex> lock(install_mtx);
        if (g != gen.load()) return;   // a newer request superseded it
        Slot* s = new Slot{std::move(v), g};
        delete pending.exchange(s);    // an older, never-played voice
    }
    void free_retired() {              // user thread
        Slot* s;
        while (retired.pop(s)) delete s;
    }
    void update_info() {               // caller holds info_mtx
        if (!info_dirty) return;
        info_dirty = false;
        BodyDesc t = target();
        describe(obj, obj_size, sizeof(obj_size));
        describe(t, tgt_size, sizeof(tgt_size));
        describe_thickness(obj, obj_thick, sizeof(obj_thick));
        describe_thickness(t, tgt_thick, sizeof(tgt_thick));
        describe_hit(obj, obj_hit, sizeof(obj_hit));
        describe_hit(t, tgt_hit, sizeof(tgt_hit));
        if (source == S_SAME) {        // the Tgt dials are ignored: say so
            for (char* c : {tgt_size, tgt_thick, tgt_hit})
                std::snprintf(c, 32, "same as object");
        }
    }
    void mark_info() {
        std::lock_guard<std::mutex> lock(info_mtx);
        info_dirty = true;
    }
    // user thread: what plays for the wanted Motion, and why it differs
    void update_action() {
        pa::ActionCheck c = pa::check_action(obj, target(), static_cast<pa::Action>(wanted_motion));
        motion.store(c.use == pa::Action::None ? M_NONE : static_cast<int>(c.use));
        std::lock_guard<std::mutex> lock(info_mtx);
        std::snprintf(motion_reason, sizeof(motion_reason), "%s", c.reason);
    }
    void rebuild() {                   // user thread, after any body change
        free_retired();
        mark_info();   // labels cost a body build each: not on every setter
        update_action();
        const int g = gen.fetch_add(1) + 1;
        const BodyDesc t = target();
        // roll / slide tables only when the motion needs them (~0.5 s cold)
        const int m = motion.load();
        built_motion = m == M_ROLL || m == M_SLIDE;
        if (pa::desc_assets_ready(obj, t, fs, built_motion)) {
            install(pa::make_desc_voice(obj, t, fs, built_motion), g);   // ~ms
            return;
        }
        builder.post(this, obj, t, fs, built_motion, g);
    }
    void set_motion(int m) {           // user thread: the wanted Motion
        wanted_motion = m;
        update_action();
        const int use = motion.load();
        if ((use == M_ROLL || use == M_SLIDE) && !built_motion) rebuild();
    }
    // user thread (release): save this Play as a WAV, keep the newest 20
    void write_capture() {
        if (cap.empty()) return;
        std::size_t n = cap_n, last = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (std::abs(cap[i]) > 1e-6f) last = i + 1;
        if (last == 0) return;                                    // never sounded
        n = std::min(n, last + static_cast<std::size_t>(0.05 * fs));
        namespace fsys = std::filesystem;
        std::error_code ec;
        const fsys::path dir = capture_dir();
        fsys::create_directories(dir, ec);
        std::time_t now = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
        static std::atomic<unsigned> seq{0};
        const int mm = motion.load();
        const char* mname = mm == M_NONE ? "None" : kMotionNames[mm];
        char name[200];
        std::snprintf(name, sizeof(name), "render_%s_%03u_%s_%s_%s.wav", stamp, seq++ % 1000,
                      mname, slug_of(obj).c_str(),
                      source == S_SAME ? "same" : slug_of(tgt).c_str());
        const fsys::path file = dir / name;
        write_wav_f32(file, cap.data(), n, static_cast<int>(fs));
        std::ofstream(dir / "last_render.txt") << file.generic_string();
        // the Play's peak, for the ProcAudio panel: a physically quiet pair
        // (a rubber ball on a massive steel ball: -55 dBFS) sounds "silent"
        float pk = 0.0f;
        for (std::size_t i = 0; i < n; ++i) pk = std::max(pk, std::abs(cap[i]));
        char peak[64];
        std::snprintf(peak, sizeof(peak), "%.1f", 20.0 * std::log10(std::max(pk, 1e-12f)));
        std::ofstream(dir / "last_play.txt") << peak << "\n" << file.generic_string() << "\n";
        // plays.txt: one line per Play, its peak and the parameter values
        // that made it, so the panel finds the peak of the SELECTED
        // instrument (an event plays all its instruments at once)
        {
            char line[400];   // the deck's values (Tgt ones even when Same as object)
            std::snprintf(line, sizeof(line),
                          "%s %d %d %d %.6g %.6g %.6g %d %d %d %d %.6g %.6g %.6g %d %.6g %d %.6g",
                          peak, static_cast<int>(obj.shape), material_index(obj.material),
                          static_cast<int>(obj.mount), obj.size, obj.thickness, obj.hit,
                          source == S_SAME ? 0 : static_cast<int>(tgt.shape) + 1,
                          material_index(tgt.material), static_cast<int>(tgt.finish),
                          static_cast<int>(tgt.mount), tgt.size, tgt.thickness, tgt.hit,
                          wanted_motion, static_cast<double>(speed.load()), render.load(),
                          static_cast<double>(level_db.load()));
            std::vector<std::string> lines;
            {
                std::ifstream in(dir / "plays.txt");
                for (std::string l; std::getline(in, l);)
                    if (!l.empty()) lines.push_back(l);
            }
            lines.push_back(line);
            if (lines.size() > 100) lines.erase(lines.begin(), lines.end() - 100);
            std::ofstream outp(dir / "plays.txt");
            for (const auto& l : lines) outp << l << "\n";
        }
        // keep the newest 20 renders
        std::vector<fsys::path> all;
        for (const auto& e : fsys::directory_iterator(dir, ec))
            if (e.path().filename().string().rfind("render_", 0) == 0) all.push_back(e.path());
        std::sort(all.begin(), all.end());
        for (std::size_t i = 0; i + 20 < all.size(); ++i) fsys::remove(all[i], ec);
    }
};

void Builder::run(State* s) {
    for (;;) {
        Req r;
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait(lock, [this] { return has_ || stop_; });
            // Studio sets a Play's parameters one by one: wait until they
            // stop coming (3 ms quiet), then build only the last request
            do {
                if (stop_) return;
                r = req_;
                has_ = false;
            } while (cv_.wait_for(lock, std::chrono::milliseconds(3),
                                  [this] { return has_ || stop_; }));
        }
        if (r.gen != s->gen.load()) continue;   // already superseded
        s->install(pa::make_desc_voice(r.o, r.t, r.fs, r.motion), r.gen);
    }
}

FMOD_DSP_PARAMETER_DESC g_param_store[P_COUNT];
FMOD_DSP_PARAMETER_DESC* g_params[P_COUNT];

State* st(FMOD_DSP_STATE* s) { return static_cast<State*>(s->plugindata); }

void queue_start(State* state) {
    const int m = state->motion.load();
    const float v = state->speed.load();
    if (m == M_IMPACT && v > 0.0f) state->events.push({0, v, 0});
    if (m == M_DROP && v > 0.0f) state->events.push({1, v, 0});
}

void retire(State* state, Slot* s) {   // mixer thread
    if (s && !state->retired.push(s)) delete s;   // queue full: extremely rare
}

// ---------------------------------------------------------- callbacks

FMOD_RESULT F_CALL cb_create(FMOD_DSP_STATE* s) {
    auto* state = new (std::nothrow) State;
    if (!state) return FMOD_ERR_MEMORY;
    int rate = 48000;
    s->functions->getsamplerate(s, &rate);
    state->fs = rate;
    s->plugindata = state;
    if (capture_enabled())   // up to 30 s of this Play (Studio only: ~6 MB)
        state->cap.assign(static_cast<std::size_t>(30.0 * state->fs), 0.0f);
    // the deck's dropdowns start at the defaults (a game override set back to
    // -1 returns to these until the deck sends its own values)
    state->deck[P_O_SHAPE] = static_cast<int>(state->obj.shape);
    state->deck[P_O_MATERIAL] = material_index(state->obj.material);
    state->deck[P_O_MOUNT] = static_cast<int>(state->obj.mount);
    state->deck[P_T_SHAPE] = state->source == S_SAME ? 0 : static_cast<int>(state->tgt.shape) + 1;
    state->deck[P_T_MATERIAL] = material_index(state->tgt.material);
    state->deck[P_T_FINISH] = static_cast<int>(state->tgt.finish);
    state->deck[P_T_MOUNT] = static_cast<int>(state->tgt.mount);
    state->deck[P_MOTION] = state->wanted_motion;
    state->deck[P_RENDER] = state->render.load();
    state->rebuild();
    return FMOD_OK;
}

FMOD_RESULT F_CALL cb_release(FMOD_DSP_STATE* s) {
    State* state = st(s);
    state->builder.shutdown();
    try { state->write_capture(); } catch (...) {}   // a preview must never break a Play
    delete state->pending.exchange(nullptr);
    delete state->active;
    delete state->fading;
    state->free_retired();
    delete state;
    return FMOD_OK;
}

FMOD_RESULT F_CALL cb_reset(FMOD_DSP_STATE* s) {
    st(s)->start_pending.store(true);   // a re-triggered instrument plays again
    st(s)->play_pos.store(0);           // ... and its Fit Length counts from here
    return FMOD_OK;
}

// FMOD calls this when playback starts part-way into the sound: Fit Length
// then counts from that position, not from 0
FMOD_RESULT F_CALL cb_setposition(FMOD_DSP_STATE* s, unsigned int pos) {
    st(s)->play_pos.store(static_cast<long long>(pos));
    return FMOD_OK;
}

// Decaying modal states turn into denormals, which cost ~100x on x86 (a
// ringing-out box impact measured 2 % -> 280 % of a core). Flush them to
// zero while a block renders (FTZ | DAZ), then restore the host's mode.
struct DenormalGuard {
#if PA_HAVE_SSE
    unsigned old = _mm_getcsr();
    DenormalGuard() { _mm_setcsr(old | 0x8040u); }
    ~DenormalGuard() { _mm_setcsr(old); }
#endif
};

FMOD_RESULT F_CALL cb_process(FMOD_DSP_STATE* s, unsigned int length,
                              const FMOD_DSP_BUFFER_ARRAY* /*in*/, FMOD_DSP_BUFFER_ARRAY* out,
                              FMOD_BOOL /*inputsidle*/, FMOD_DSP_PROCESS_OPERATION op) {
    State* state = st(s);
    if (op == FMOD_DSP_PROCESS_QUERY) {
        if (out) {
            out->speakermode = FMOD_SPEAKERMODE_MONO;
            out->buffernumchannels[0] = 1;
        }
        return FMOD_OK;
    }
    DenormalGuard ftz;
    const long long pos0 = state->play_pos.fetch_add(length);   // this block's first sample
    const double fit = state->fit_len.load();
    const bool fitted = fit > 0.0;
    // a freshly built voice takes over; the old one rings out alongside
    if (Slot* fresh = state->pending.exchange(nullptr)) {
        if (state->active) {
            retire(state, state->fading);
            state->fading = state->active;
            state->fading->v->set_motion(pa::Motion::None);
            state->fading->v->set_target_speed(0.0);
            state->fade_quiet = state->fade_age = 0;
        }
        state->active = fresh;
    }
    float* buf = out->buffers[0];
    const int nch = out->buffernumchannels[0];
    Slot* a = state->active;
    if (!a) {   // still building: keep the start event for when it arrives
        std::memset(buf, 0, sizeof(float) * length * nch);
        return FMOD_OK;
    }
    pa::ContactVoice* v = a->v.get();
    // the start hit belongs to the CURRENT settings: wait for their voice
    const bool current = a->gen == state->gen.load();
    if (current && state->start_pending.exchange(false)) queue_start(state);

    const int mode = state->motion.load();   // the played motion (check_action)
    pa::Motion m = pa::Motion::None;
    if (mode == M_ROLL) m = v->roll() ? pa::Motion::Roll : pa::Motion::Slide;
    if (mode == M_SLIDE) m = pa::Motion::Slide;
    v->set_motion(m);
    v->set_target_speed(m == pa::Motion::None ? 0.0 : state->speed.load());
    Event e;
    bool any_event = false;
    while (state->events.pop(e)) {
        int off = std::min<int>(e.offset, static_cast<int>(length) - 1);
        if (e.kind == 0) v->impact(e.v, off);
        else v->drop(e.v, off);
        any_event = true;
    }
    const double gain = std::pow(10.0, state->level_db.load() / 20.0) / kFullScalePa;
    const int render = state->render.load();
    const double wo = render == R_TARGET ? 0.0 : 1.0, wt = render == R_OBJECT ? 0.0 : 1.0;
    double ob[512], tb[512];
    unsigned done = 0;
    double fade_ss = 0.0;
    float peak = 0.0f;
    while (done < length) {
        const int n = static_cast<int>(std::min(512u, length - done));
        double mix[512];
        v->process_stems(ob, tb, n);
        for (int i = 0; i < n; ++i) mix[i] = wo * ob[i] + wt * tb[i];
        if (state->fading) {
            state->fading->v->process_stems(ob, tb, n);
            for (int i = 0; i < n; ++i) {
                double y = wo * ob[i] + wt * tb[i];
                mix[i] += y;
                fade_ss += y * y;
            }
        }
        for (int i = 0; i < n; ++i) {
            double g = gain;
            if (fitted) g *= fit_gain(static_cast<double>(pos0 + done + i) / state->fs, fit);
            const float y = static_cast<float>(g * mix[i]);
            peak = std::max(peak, std::abs(y));
            for (int c = 0; c < nch; ++c) buf[(done + i) * nch + c] = y;
        }
        if (state->cap_n < state->cap.size()) {   // preview capture (preallocated)
            const std::size_t k = std::min<std::size_t>(n, state->cap.size() - state->cap_n);
            for (std::size_t i = 0; i < k; ++i) state->cap[state->cap_n + i] = buf[(done + i) * nch];
            state->cap_n += k;
        }
        done += static_cast<unsigned>(n);
    }
    if (state->fading) {   // done when 100 ms below -120 dB re 63 Pa (30 s at most)
        const bool quiet = gain * gain * fade_ss / length < 1e-12;
        state->fade_quiet = quiet ? state->fade_quiet + length : 0;
        state->fade_age += length;
        if (state->fade_quiet > 0.1 * state->fs || state->fade_age > 30.0 * state->fs) {
            retire(state, state->fading);
            state->fading = nullptr;
        }
    }
    // the sound has ended when it is QUIET (below -120 dBFS for 100 ms) with
    // nothing left to come: no motion, no event, no drop or old voice still
    // running. A fixed timer would cut long rings (a steel plate rings > 3 s).
    const bool moving = m != pa::Motion::None && state->speed.load() > 0.0f;
    // Studio ends a plug-in instrument once it reports silence, so never
    // before this Play's own voice has fired its start: a voice still being
    // built (or an old one waiting for its replacement) is silent too.
    const bool started = current && !state->start_pending.load();
    // past the Fit Length the output is 0 whatever still rings or moves
    const bool past_fit = fitted && static_cast<double>(pos0 + length) >= fit * state->fs;
    const bool quiet = past_fit || (started && peak < 1e-6f && !moving && !any_event &&
                                    !v->busy() && !state->fading);
    state->quiet_samples = quiet ? state->quiet_samples + length : 0;
    return state->quiet_samples > 0.1 * state->fs ? FMOD_ERR_DSP_SILENCE : FMOD_OK;
}

BodyDesc* side(State* state, int index) {
    return index >= P_T_SHAPE ? &state->tgt : &state->obj;
}

FMOD_RESULT apply_int(State* state, int index, int value) {
    BodyDesc* d = side(state, index);
    bool changed = false;
    switch (index) {
        case P_O_SHAPE: {
            auto v = static_cast<pa::Shape>(std::clamp(value, 0, static_cast<int>(pa::Shape::Count) - 1));
            changed = v != d->shape; d->shape = v; break;
        }
        case P_T_SHAPE: {   // 0 = Same as object, 1.. = the shapes
            const int v = std::clamp(value, 0, static_cast<int>(pa::Shape::Count));
            const int src = v == 0 ? S_SAME : S_DECK;
            changed = src != state->source;
            state->source = src;
            if (v > 0) {
                auto sh = static_cast<pa::Shape>(v - 1);
                changed |= sh != d->shape;
                d->shape = sh;
            }
            if (changed) state->rebuild();
            return FMOD_OK;
        }
        case P_O_MATERIAL: case P_T_MATERIAL: {
            std::string v = pa::body_material(value);
            changed = v != d->material; d->material = v; break;
        }
        case P_T_FINISH: {
            auto v = static_cast<pa::Finish>(std::clamp(value, 0, static_cast<int>(pa::Finish::Count) - 1));
            changed = v != d->finish; d->finish = v; break;
        }
        case P_O_MOUNT: case P_T_MOUNT: {
            auto v = static_cast<pa::Mount>(std::clamp(value, 0, static_cast<int>(pa::Mount::Count) - 1));
            changed = v != d->mount; d->mount = v; break;
        }
        case P_MOTION: state->set_motion(std::clamp(value, 0, M_COUNT - 1)); return FMOD_OK;   // wanted
        case P_RENDER: state->render.store(std::clamp(value, 0, R_COUNT - 1)); return FMOD_OK;
        default: return FMOD_ERR_INVALID_PARAM;
    }
    // target settings do not matter while the target is the object
    const bool silent_target = index > P_T_SHAPE && state->source == S_SAME;
    if (changed && !silent_target) state->rebuild();
    else if (changed) state->mark_info();
    return FMOD_OK;
}

int game_slot(int index) {   // the game parameter overriding this dropdown, or -1
    for (int k = 0; k < N_GAME; ++k)
        if (kGameTarget[k] == index) return k;
    return -1;
}

FMOD_RESULT F_CALL cb_setint(FMOD_DSP_STATE* s, int index, int value) {
    State* state = st(s);
    state->free_retired();
    if (index < 0 || index >= P_COUNT) return FMOD_ERR_INVALID_PARAM;
    state->deck[index] = value;
    const int k = game_slot(index);
    if (k >= 0 && state->game[k] > -0.5f) return FMOD_OK;   // a game override wins
    return apply_int(state, index, value);
}

// game control (P_G_*): an override, or back to the deck's value
FMOD_RESULT set_game(State* state, int index, float value) {
    const int k = index - P_GAME_FIRST;
    state->game[k] = value;
    const int target = kGameTarget[k];
    const int v = value > -0.5f ? static_cast<int>(std::lround(value)) : state->deck[target];
    return apply_int(state, target, v);
}

FMOD_RESULT F_CALL cb_getint(FMOD_DSP_STATE* s, int index, int* value, char* str) {
    State* state = st(s);
    const BodyDesc* d = side(state, index);
    const char* name = nullptr;
    switch (index) {
        case P_O_SHAPE:
            *value = static_cast<int>(d->shape); name = pa::shape_name(d->shape); break;
        case P_T_SHAPE:
            *value = state->source == S_SAME ? 0 : static_cast<int>(d->shape) + 1;
            name = g_target_names[*value]; break;
        case P_O_MATERIAL: case P_T_MATERIAL:
            *value = material_index(d->material); name = pa::body_material(*value); break;
        case P_T_FINISH:
            *value = static_cast<int>(d->finish); name = pa::finish_name(d->finish); break;
        case P_O_MOUNT: case P_T_MOUNT:
            *value = static_cast<int>(d->mount); name = pa::mount_name(d->mount); break;
        case P_MOTION: *value = state->wanted_motion; name = kMotionNames[*value]; break;
        case P_RENDER: *value = state->render.load(); name = kRenderNames[*value]; break;
        default: return FMOD_ERR_INVALID_PARAM;
    }
    if (str) std::snprintf(str, FMOD_DSP_GETPARAM_VALUESTR_LENGTH, "%s", name);
    return FMOD_OK;
}

FMOD_RESULT F_CALL cb_setfloat(FMOD_DSP_STATE* s, int index, float value) {
    State* state = st(s);
    state->free_retired();
    BodyDesc* d = side(state, index);
    double* field = nullptr;
    switch (index) {
        case P_O_SIZE: case P_T_SIZE: field = &d->size; break;
        case P_O_THICK: case P_T_THICK: field = &d->thickness; break;
        case P_O_HIT: case P_T_HIT: field = &d->hit; break;
        case P_LEVEL: state->level_db.store(value); return FMOD_OK;
        case P_FIT: state->fit_len.store(std::max(0.0f, value)); return FMOD_OK;
        case P_G_MOTION: case P_G_TARGET: case P_G_T_MATERIAL:
        case P_G_T_FINISH: case P_G_T_MOUNT: case P_G_O_MOUNT:
            return set_game(state, index, value);
        case P_SPEED: {
            const bool changed = value != state->speed.exchange(value);
            // Impact / Drop: a NEW value re-fires the one-shot (automation,
            // game parameters); the instrument start fires it once anyway
            // (start_pending). Roll / Slide: just the new target speed.
            if (changed && value > 0.0f && !state->start_pending.load()) {
                int m = state->motion.load();
                if (m == M_IMPACT) state->events.push({0, value, 0});
                if (m == M_DROP) state->events.push({1, value, 0});
            }
            return FMOD_OK;
        }
        default: return FMOD_ERR_INVALID_PARAM;
    }
    // compare at float precision: Studio sets every parameter on a new
    // instance, as floats (0.06 arrives as 0.0599999986); comparing against
    // the double default would rebuild the voice for nothing, once per dial
    if (static_cast<float>(*field) == value) return FMOD_OK;
    *field = value;
    if (index >= P_T_SHAPE && state->source == S_SAME) state->mark_info();
    else state->rebuild();
    return FMOD_OK;
}

FMOD_RESULT F_CALL cb_getfloat(FMOD_DSP_STATE* s, int index, float* value, char* str) {
    State* state = st(s);
    const BodyDesc* d = side(state, index);
    const char* info = nullptr;
    std::lock_guard<std::mutex> lock(state->info_mtx);
    if (str) state->update_info();
    switch (index) {
        case P_O_SIZE: *value = static_cast<float>(d->size); info = state->obj_size; break;
        case P_T_SIZE: *value = static_cast<float>(d->size); info = state->tgt_size; break;
        case P_O_THICK: *value = static_cast<float>(d->thickness); info = state->obj_thick; break;
        case P_T_THICK: *value = static_cast<float>(d->thickness); info = state->tgt_thick; break;
        case P_O_HIT: *value = static_cast<float>(d->hit); info = state->obj_hit; break;
        case P_T_HIT: *value = static_cast<float>(d->hit); info = state->tgt_hit; break;
        case P_SPEED: *value = state->speed.load(); break;
        case P_LEVEL: *value = state->level_db.load(); break;
        case P_FIT: *value = state->fit_len.load(); break;
        case P_G_MOTION: case P_G_TARGET: case P_G_T_MATERIAL:
        case P_G_T_FINISH: case P_G_T_MOUNT: case P_G_O_MOUNT:
            *value = state->game[index - P_GAME_FIRST];
            info = *value > -0.5f ? "game" : "off (deck)";
            break;
        default: return FMOD_ERR_INVALID_PARAM;
    }
    char fit_info[32];
    if (index == P_FIT) {
        if (*value <= 0.0f) std::snprintf(fit_info, sizeof(fit_info), "off: rings out");
        else std::snprintf(fit_info, sizeof(fit_info), "ends at %.3g s (%.0f ms fade)", *value,
                           1000.0 * fit_fade_s(*value));
        info = fit_info;
    }
    char speed_info[32];
    if (index == P_SPEED) {   // what the one Speed knob means right now
        const int m = state->motion.load();
        const double v = *value;
        if (m == M_IMPACT) std::snprintf(speed_info, sizeof(speed_info), "hit at %.2g m/s", v);
        else if (m == M_DROP)   // v^2 = 2 g h
            std::snprintf(speed_info, sizeof(speed_info), "drop %.2g m/s = from %.0f cm", v,
                          100.0 * v * v / (2.0 * 9.81));
        else std::snprintf(speed_info, sizeof(speed_info), "%s at %.2g m/s",
                           m == M_ROLL ? "rolling" : "sliding", v);
        if (state->motion_reason[0])   // e.g. "a held ball can't turn"
            std::snprintf(speed_info, sizeof(speed_info), "%s", state->motion_reason);
        info = speed_info;
    }
    if (str) {
        if (info) std::snprintf(str, FMOD_DSP_GETPARAM_VALUESTR_LENGTH, "%s", info);
        else std::snprintf(str, FMOD_DSP_GETPARAM_VALUESTR_LENGTH, "%.2f", *value);
    }
    return FMOD_OK;
}

FMOD_RESULT F_CALL cb_setdata(FMOD_DSP_STATE* s, int index, void* data, unsigned int length) {
    if (index != P_CONTACTS) return FMOD_ERR_INVALID_PARAM;
    State* state = st(s);
    const auto* msg = static_cast<const ContactMsg*>(data);
    const unsigned n = length / sizeof(ContactMsg);
    for (unsigned i = 0; i < n; ++i) {
        const ContactMsg& c = msg[i];
        int off = std::max(0, static_cast<int>(c.t_offset_s * state->fs));
        if (c.kind == 0) state->events.push({0, c.v_normal, off});
        else if (c.kind == 4) state->events.push({1, c.v_normal, off});
        else if (c.kind == 1) { state->set_motion(M_SLIDE); state->speed.store(c.v_tangent); }
        else if (c.kind == 2) { state->set_motion(M_ROLL); state->speed.store(c.v_tangent); }
        else state->speed.store(0.0f);
    }
    return FMOD_OK;
}

FMOD_RESULT F_CALL cb_getdata(FMOD_DSP_STATE*, int, void** data, unsigned int* length, char*) {
    *data = nullptr;
    *length = 0;
    return FMOD_OK;
}

// prewarm the asset cache once FMOD registers the plug-in (Studio: at
// start-up): the old presets as descriptions, so that common first Plays
// (and the deck default) start in the first mix block
FMOD_RESULT F_CALL cb_sys_register(FMOD_DSP_STATE* s) {
    Prewarm& p = prewarm();
    std::lock_guard<std::mutex> lock(p.mtx);
    if (p.users++ == 0) {
        int rate = 48000;
        if (s && s->functions && s->functions->getsamplerate) s->functions->getsamplerate(s, &rate);
        double fs = rate > 0 ? rate : 48000;
        p.stop = false;
        p.th = std::thread([fs] { pa::prewarm_desc_presets(fs, &prewarm().stop); });
    }
    return FMOD_OK;
}

FMOD_RESULT F_CALL cb_sys_deregister(FMOD_DSP_STATE*) {
    Prewarm& p = prewarm();
    std::lock_guard<std::mutex> lock(p.mtx);
    if (--p.users == 0) {
        p.stop = true;
        if (p.th.joinable()) p.th.join();
    }
    return FMOD_OK;
}

FMOD_DSP_DESCRIPTION g_desc = {
    FMOD_PLUGIN_SDK_VERSION,
    "ProcAudio Contact",   // name: company/product, no version
    0x00020500,            // plug-in version 2.5 (2.5: game control parameters)
    0,                     // no input: a sound module
    1,                     // one output buffer
    cb_create, cb_release, cb_reset,
    nullptr,               // read
    cb_process,
    cb_setposition,
    P_COUNT, g_params,
    cb_setfloat, cb_setint, nullptr, cb_setdata,
    cb_getfloat, cb_getint, nullptr, cb_getdata,
    nullptr,               // shouldiprocess (process() reports silence itself)
    nullptr,               // userdata
    cb_sys_register, cb_sys_deregister, nullptr};

}  // namespace

extern "C" F_EXPORT FMOD_DSP_DESCRIPTION* F_CALL FMODGetDSPDescription() {
    static bool init = false;
    if (!init) {
        const int n_shapes = static_cast<int>(pa::Shape::Count);
        const int n_finish = static_cast<int>(pa::Finish::Count);
        const int n_mount = static_cast<int>(pa::Mount::Count);
        for (int i = 0; i < n_shapes; ++i) g_shape_names[i] = pa::shape_name(static_cast<pa::Shape>(i));
        g_target_names[0] = "Same as object";
        for (int i = 0; i < n_shapes; ++i) g_target_names[i + 1] = g_shape_names[i];
        for (int i = 0; i < pa::num_body_materials(); ++i) g_material_names[i] = pa::body_material(i);
        for (int i = 0; i < n_finish; ++i) g_finish_names[i] = pa::finish_name(static_cast<pa::Finish>(i));
        for (int i = 0; i < n_mount; ++i) g_mount_names[i] = pa::mount_name(static_cast<pa::Mount>(i));
        auto& P = g_param_store;
        const BodyDesc o = default_object(), t = default_target();
        const int n_mat = pa::num_body_materials();
        // object
        FMOD_DSP_INIT_PARAMDESC_INT(P[P_O_SHAPE], "Obj Shape", "", "Object shape", 0, n_shapes - 1,
                                    static_cast<int>(o.shape), false, g_shape_names);
        FMOD_DSP_INIT_PARAMDESC_INT(P[P_O_MATERIAL], "Obj Material", "", "Object material", 0,
                                    n_mat - 1, material_index(o.material), false, g_material_names);
        FMOD_DSP_INIT_PARAMDESC_INT(P[P_O_MOUNT], "Obj Mount", "", "How the object is held", 0,
                                    n_mount - 1, static_cast<int>(o.mount), false, g_mount_names);
        FMOD_DSP_INIT_PARAMDESC_FLOAT_WITH_MAPPING(P[P_O_SIZE], "Obj Size", "m",
            "Ball diameter, stick length, box side, panel / slab span",
            static_cast<float>(o.size), g_size_vals, g_size_pos);
        FMOD_DSP_INIT_PARAMDESC_FLOAT_WITH_MAPPING(P[P_O_THICK], "Obj Thickness", "m",
            "Stick section, box wall, plate thickness (0 = auto)",
            static_cast<float>(o.thickness), g_thick_vals, g_thick_pos);
        FMOD_DSP_INIT_PARAMDESC_FLOAT(P[P_O_HIT], "Obj Hit Pos", "",
                                      "Where the contact is: 0 center / middle, 1 edge / end",
                                      0.0f, 1.0f, static_cast<float>(o.hit));
        // target
        FMOD_DSP_INIT_PARAMDESC_INT(P[P_T_SHAPE], "Target", "",
                                    "What the object meets: the same as the object, or a shape",
                                    0, n_shapes, static_cast<int>(t.shape) + 1, false, g_target_names);
        FMOD_DSP_INIT_PARAMDESC_INT(P[P_T_MATERIAL], "Tgt Material", "", "Target material", 0,
                                    n_mat - 1, material_index(t.material), false, g_material_names);
        FMOD_DSP_INIT_PARAMDESC_INT(P[P_T_FINISH], "Tgt Finish", "",
                                    "Floor surface (Panel, Ground only): what the object hits, "
                                    "lands, rolls or slides on", 0,
                                    n_finish - 1, static_cast<int>(t.finish), false, g_finish_names);
        FMOD_DSP_INIT_PARAMDESC_INT(P[P_T_MOUNT], "Tgt Mount", "", "How the target is held", 0,
                                    n_mount - 1, static_cast<int>(t.mount), false, g_mount_names);
        FMOD_DSP_INIT_PARAMDESC_FLOAT_WITH_MAPPING(P[P_T_SIZE], "Tgt Size", "m",
            "Ball diameter, stick length, box side, panel / slab span",
            static_cast<float>(t.size), g_size_vals, g_size_pos);
        FMOD_DSP_INIT_PARAMDESC_FLOAT_WITH_MAPPING(P[P_T_THICK], "Tgt Thickness", "m",
            "Stick section, box wall, plate thickness (0 = auto)",
            static_cast<float>(t.thickness), g_thick_vals, g_thick_pos);
        FMOD_DSP_INIT_PARAMDESC_FLOAT(P[P_T_HIT], "Tgt Hit Pos", "",
                                      "Where the contact is: 0 center / middle, 1 edge / end",
                                      0.0f, 1.0f, static_cast<float>(t.hit));
        // motion and output
        FMOD_DSP_INIT_PARAMDESC_INT(P[P_MOTION], "Motion", "", "Impact, drop, roll or slide", 0,
                                    M_COUNT - 1, M_IMPACT, false, kMotionNames);
        FMOD_DSP_INIT_PARAMDESC_FLOAT_WITH_MAPPING(P[P_SPEED], "Speed", "m/s",
            "Impact / Drop: hit or landing speed. Roll / Slide: rolling or sliding speed",
            2.0f, g_speed_vals, g_speed_pos);
        FMOD_DSP_INIT_PARAMDESC_INT(P[P_RENDER], "Render", "", "Which bodies this event plays", 0,
                                    R_COUNT - 1, R_BOTH, false, kRenderNames);
        FMOD_DSP_INIT_PARAMDESC_FLOAT(P[P_LEVEL], "Level", "dB",
                                      "Gain over the calibration 0 dBFS = 63 Pa (130 dB SPL) at 1 m",
                                      -40.0f, 80.0f, 0.0f);
        FMOD_DSP_INIT_PARAMDESC_DATA(P[P_CONTACTS], "Contacts", "", "ContactMsg[] from game code",
                                     FMOD_DSP_PARAMETER_DATA_TYPE_USER);
        FMOD_DSP_INIT_PARAMDESC_FLOAT_WITH_MAPPING(P[P_FIT], "Fit Length", "s",
            "Fade to silence at this time after the start: the instrument's length, written by "
            "ProcAudio > Ending: Fit (0 = off, the sound rings out)",
            0.0f, g_fit_vals, g_fit_pos);
        // game control: plain linear floats, so FMOD Studio automation and
        // game parameters set them exactly (-1 = off: the deck decides)
        FMOD_DSP_INIT_PARAMDESC_FLOAT(P[P_G_MOTION], "Game Motion", "",
            "Game control, overrides Motion: -1 off, 0 Impact, 1 Drop, 2 Roll, 3 Slide",
            -1.0f, static_cast<float>(M_COUNT - 1), -1.0f);
        FMOD_DSP_INIT_PARAMDESC_FLOAT(P[P_G_TARGET], "Game Target", "",
            "Game control, overrides Target: -1 off, 0 Same as object, 1 Ball .. 5 Ground",
            -1.0f, static_cast<float>(n_shapes), -1.0f);
        FMOD_DSP_INIT_PARAMDESC_FLOAT(P[P_G_T_MATERIAL], "Game Tgt Mat", "",
            "Game control, overrides Tgt Material: -1 off, else the material index",
            -1.0f, static_cast<float>(pa::num_body_materials() - 1), -1.0f);
        FMOD_DSP_INIT_PARAMDESC_FLOAT(P[P_G_T_FINISH], "Game Tgt Finish", "",
            "Game control, overrides Tgt Finish: -1 off, 0 Bare, 1 Polished, 2 Tiled, 3 Carpeted",
            -1.0f, static_cast<float>(static_cast<int>(pa::Finish::Count) - 1), -1.0f);
        FMOD_DSP_INIT_PARAMDESC_FLOAT(P[P_G_T_MOUNT], "Game Tgt Mount", "",
            "Game control, overrides Tgt Mount: -1 off, 0 Free, 1 Held, 2 Resting, 3 Built-in",
            -1.0f, static_cast<float>(static_cast<int>(pa::Mount::Count) - 1), -1.0f);
        FMOD_DSP_INIT_PARAMDESC_FLOAT(P[P_G_O_MOUNT], "Game Obj Mount", "",
            "Game control, overrides Obj Mount: -1 off, 0 Free, 1 Held, 2 Resting, 3 Built-in",
            -1.0f, static_cast<float>(static_cast<int>(pa::Mount::Count) - 1), -1.0f);
        for (int i = 0; i < P_COUNT; ++i) g_params[i] = &P[i];
        init = true;
    }
    return &g_desc;
}
