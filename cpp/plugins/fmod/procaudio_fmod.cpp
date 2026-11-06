// FMOD sound module (generator: 0 inputs, 1 mono output) wrapping one
// pa::ContactVoice from the preset catalog. Built against the FMOD Engine
// 2.02.23 SDK (to match FMOD Studio 2.02.23); integration-tested in the real
// FMOD engine by cpp/tests/fmod_host_check.py.
//
// Parameters (procaudio_fmod.plugin.js lays them out in Studio's deck):
//   Object  int    wood ball S/L, steel ball S/L, cardboard box S/L, steel box S/L
//   Surface int    varnished wood, wood floor, concrete, tile, rug
//   Motion  int    Impact: one hit at "Impact" m/s when the instrument starts
//                  Drop:   a dropped object -- balls bounce, boxes rock on
//                          their edges and settle (Housner rocking)
//                  Roll / Slide: continuous at "Speed", no hits
//   Speed   float  m/s, roll/slide speed (smoothed, 80 ms)
//   Impact  float  m/s, hit / landing speed; in Impact and Drop modes it
//                  fires when the instrument starts and on every new value
//   Level   float  dB over the calibration 0 dBFS = 63 Pa at 1 m (130 dB SPL):
//                  the loudest preset impact at its default speed just fits.
//                  The physics spans ~75 dB (hard impacts ~129 dB SPL peak,
//                  rolling 51-76 dB, sliding 51-64 dB), so roll / slide events
//                  need Level raised (+35 / +50 presets in the .plugin.js).
//   Contacts data  ContactMsg[] from game code (sample-accurate offsets)
//
// Threading: FMOD calls create/release/setters on the user thread and
// process() on the mixer thread. Voices come from the process-wide asset
// cache (catalog.h): synchronous and ~ms when warm; the cache is prewarmed
// on a worker thread when FMOD registers the plug-in. A cold build runs on
// one worker thread per instance, joined before the next build and in
// release(). Speed/Impact/Contacts go through atomics and a lock-free
// single-producer/single-consumer queue.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>

#include "fmod.h"
#include "fmod_dsp.h"
#include "procaudio/catalog.h"

namespace {

const char* const kMotionNames[] = {"Impact", "Drop", "Roll", "Slide"};
enum MotionMode { M_IMPACT, M_DROP, M_ROLL, M_SLIDE, M_COUNT };
constexpr double kFullScalePa = 63.0;  // 0 dBFS = 63 Pa at 1 m (130 dB SPL)

// What game code sends through the "Contacts" data parameter.
struct ContactMsg {
    uint32_t object_id, surface_id;
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

class EventQueue {   // SPSC: user thread pushes, mixer thread pops
public:
    bool push(Event e) {
        unsigned h = head_.load(std::memory_order_relaxed), n = (h + 1) % kSize;
        if (n == tail_.load(std::memory_order_acquire)) return false;
        buf_[h] = e;
        head_.store(n, std::memory_order_release);
        return true;
    }
    bool pop(Event& e) {
        unsigned t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        e = buf_[t];
        tail_.store((t + 1) % kSize, std::memory_order_release);
        return true;
    }
private:
    static constexpr unsigned kSize = 256;
    Event buf_[kSize];
    std::atomic<unsigned> head_{0}, tail_{0};
};

// ---------------------------------------------------------- prewarm

struct Prewarm {
    std::mutex mtx;
    std::thread th;
    std::atomic<bool> stop{false};
    int users = 0;
};
Prewarm& prewarm() {
    static Prewarm p;
    return p;
}

// ---------------------------------------------------------- instance state

enum Param { P_OBJECT, P_SURFACE, P_MOTION, P_SPEED, P_IMPACT, P_LEVEL, P_CONTACTS, P_COUNT };

struct State {
    double fs = 48000.0;
    int object = 3, surface = 0;
    std::atomic<int> motion{M_IMPACT};
    std::atomic<float> speed{0.5f}, level_db{0.0f};
    float impact = 2.0f;
    std::atomic<bool> start_pending{true};   // fire Impact / Drop on first process
    std::atomic<pa::ContactVoice*> active{nullptr}, pending{nullptr}, retired{nullptr};
    std::thread builder;
    EventQueue events;
    unsigned idle_samples = 0;

    void install(pa::ContactVoice* v) { delete pending.exchange(v); }   // user thread
    void rebuild() {                                                    // user thread
        free_retired();
        if (builder.joinable()) builder.join();
        if (pa::preset_assets_ready(object, surface, fs)) {
            install(pa::make_preset_voice(object, surface, fs).release());   // ~ms
            return;
        }
        int obj = object, surf = surface;
        double rate = fs;
        builder = std::thread([this, obj, surf, rate] {
            install(pa::make_preset_voice(obj, surf, rate).release());
        });
    }
    void free_retired() { delete retired.exchange(nullptr); }
};

FMOD_DSP_PARAMETER_DESC p_object, p_surface, p_motion, p_speed, p_impact, p_level, p_contacts;
FMOD_DSP_PARAMETER_DESC* g_params[P_COUNT] = {&p_object, &p_surface, &p_motion, &p_speed,
                                              &p_impact, &p_level, &p_contacts};
const char* g_object_names[16];
const char* g_surface_names[16];

State* st(FMOD_DSP_STATE* s) { return static_cast<State*>(s->plugindata); }

void queue_start(State* state) {
    int m = state->motion.load();
    if (m == M_IMPACT && state->impact > 0.0f) state->events.push({0, state->impact, 0});
    if (m == M_DROP && state->impact > 0.0f) state->events.push({1, state->impact, 0});
}

// ---------------------------------------------------------- callbacks

FMOD_RESULT F_CALL cb_create(FMOD_DSP_STATE* s) {
    auto* state = new (std::nothrow) State;
    if (!state) return FMOD_ERR_MEMORY;
    int rate = 48000;
    s->functions->getsamplerate(s, &rate);
    state->fs = rate;
    s->plugindata = state;
    state->rebuild();
    return FMOD_OK;
}

FMOD_RESULT F_CALL cb_release(FMOD_DSP_STATE* s) {
    State* state = st(s);
    if (state->builder.joinable()) state->builder.join();
    delete state->pending.exchange(nullptr);
    delete state->active.exchange(nullptr);
    state->free_retired();
    delete state;
    return FMOD_OK;
}

FMOD_RESULT F_CALL cb_reset(FMOD_DSP_STATE* s) {
    st(s)->start_pending.store(true);   // a re-triggered instrument plays again
    return FMOD_OK;
}

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
    // pick up a freshly built voice (the old one is freed on the user thread)
    if (pa::ContactVoice* fresh = state->pending.exchange(nullptr)) {
        pa::ContactVoice* old = state->active.exchange(fresh);
        delete state->retired.exchange(old);   // rare; usually null
    }
    pa::ContactVoice* v = state->active.load();
    float* buf = out->buffers[0];
    const int nch = out->buffernumchannels[0];
    if (!v) {   // still building: keep the start event for when it arrives
        std::memset(buf, 0, sizeof(float) * length * nch);
        return FMOD_OK;
    }
    if (state->start_pending.exchange(false)) queue_start(state);

    const int mode = state->motion.load();
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
    float mono[1024];
    unsigned done = 0;
    while (done < length) {
        unsigned n = std::min(1024u, length - done);
        v->process(mono, static_cast<int>(n), gain);
        for (unsigned i = 0; i < n; ++i)
            for (int c = 0; c < nch; ++c) buf[(done + i) * nch + c] = mono[i];
        done += n;
    }
    // idle: no motion, no event, no drop still running -> silence after 3 s
    const bool moving = m != pa::Motion::None && state->speed.load() > 0.0f;
    if (!moving && !any_event && !v->busy()) state->idle_samples += length;
    else state->idle_samples = 0;
    return state->idle_samples > 3.0 * state->fs ? FMOD_ERR_DSP_SILENCE : FMOD_OK;
}

FMOD_RESULT F_CALL cb_setint(FMOD_DSP_STATE* s, int index, int value) {
    State* state = st(s);
    if (index == P_OBJECT) {
        if (value != state->object) { state->object = value; state->rebuild(); }
    } else if (index == P_SURFACE) {
        if (value != state->surface) { state->surface = value; state->rebuild(); }
    } else if (index == P_MOTION) {
        state->motion.store(value);
    } else {
        return FMOD_ERR_INVALID_PARAM;
    }
    return FMOD_OK;
}

FMOD_RESULT F_CALL cb_getint(FMOD_DSP_STATE* s, int index, int* value, char* str) {
    State* state = st(s);
    const char* name = nullptr;
    if (index == P_OBJECT) { *value = state->object; name = pa::object_preset_name(*value); }
    else if (index == P_SURFACE) { *value = state->surface; name = pa::surface_preset_name(*value); }
    else if (index == P_MOTION) { *value = state->motion.load(); name = kMotionNames[*value]; }
    else return FMOD_ERR_INVALID_PARAM;
    if (str) std::snprintf(str, FMOD_DSP_GETPARAM_VALUESTR_LENGTH, "%s", name);
    return FMOD_OK;
}

FMOD_RESULT F_CALL cb_setfloat(FMOD_DSP_STATE* s, int index, float value) {
    State* state = st(s);
    state->free_retired();
    if (index == P_SPEED) state->speed.store(value);
    else if (index == P_LEVEL) state->level_db.store(value);
    else if (index == P_IMPACT) {
        bool changed = value != state->impact;
        state->impact = value;
        // a NEW value re-fires the one-shot (automation, game parameters);
        // the instrument start fires it once anyway (start_pending)
        if (changed && value > 0.0f && !state->start_pending.load()) {
            int m = state->motion.load();
            if (m == M_IMPACT) state->events.push({0, value, 0});
            if (m == M_DROP) state->events.push({1, value, 0});
        }
    } else {
        return FMOD_ERR_INVALID_PARAM;
    }
    return FMOD_OK;
}

FMOD_RESULT F_CALL cb_getfloat(FMOD_DSP_STATE* s, int index, float* value, char* str) {
    State* state = st(s);
    if (index == P_SPEED) *value = state->speed.load();
    else if (index == P_LEVEL) *value = state->level_db.load();
    else if (index == P_IMPACT) *value = state->impact;
    else return FMOD_ERR_INVALID_PARAM;
    if (str) std::snprintf(str, FMOD_DSP_GETPARAM_VALUESTR_LENGTH, "%.2f", *value);
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
        else if (c.kind == 1) { state->motion.store(M_SLIDE); state->speed.store(c.v_tangent); }
        else if (c.kind == 2) { state->motion.store(M_ROLL); state->speed.store(c.v_tangent); }
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
// start-up), so that the first Play of each object/surface is fast too
FMOD_RESULT F_CALL cb_sys_register(FMOD_DSP_STATE* s) {
    Prewarm& p = prewarm();
    std::lock_guard<std::mutex> lock(p.mtx);
    if (p.users++ == 0) {
        int rate = 48000;
        if (s && s->functions && s->functions->getsamplerate) s->functions->getsamplerate(s, &rate);
        double fs = rate > 0 ? rate : 48000;
        p.stop = false;
        p.th = std::thread([fs] { pa::prewarm_presets(fs, &prewarm().stop); });
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
    0x00010200,            // plug-in version 1.2
    0,                     // no input: a sound module
    1,                     // one output buffer
    cb_create, cb_release, cb_reset,
    nullptr,               // read
    cb_process,
    nullptr,               // setposition
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
        for (int i = 0; i < pa::num_object_presets(); ++i) g_object_names[i] = pa::object_preset_name(i);
        for (int i = 0; i < pa::num_surface_presets(); ++i) g_surface_names[i] = pa::surface_preset_name(i);
        FMOD_DSP_INIT_PARAMDESC_INT(p_object, "Object", "", "Sounding object", 0,
                                    pa::num_object_presets() - 1, 3, false, g_object_names);
        FMOD_DSP_INIT_PARAMDESC_INT(p_surface, "Surface", "", "Floor it touches", 0,
                                    pa::num_surface_presets() - 1, 0, false, g_surface_names);
        FMOD_DSP_INIT_PARAMDESC_INT(p_motion, "Motion", "", "Impact, drop, roll or slide", 0,
                                    M_COUNT - 1, M_IMPACT, false, kMotionNames);
        FMOD_DSP_INIT_PARAMDESC_FLOAT(p_speed, "Speed", "m/s", "Roll / slide speed", 0.0f, 3.0f, 0.5f);
        FMOD_DSP_INIT_PARAMDESC_FLOAT(p_impact, "Impact", "m/s",
                                      "Hit / landing speed (Impact, Drop modes)", 0.0f, 10.0f, 2.0f);
        FMOD_DSP_INIT_PARAMDESC_FLOAT(p_level, "Level", "dB",
                                      "Gain over the calibration 0 dBFS = 63 Pa (130 dB SPL) at 1 m",
                                      -40.0f, 80.0f, 0.0f);
        FMOD_DSP_INIT_PARAMDESC_DATA(p_contacts, "Contacts", "", "ContactMsg[] from game code",
                                     FMOD_DSP_PARAMETER_DATA_TYPE_USER);
        init = true;
    }
    return &g_desc;
}
