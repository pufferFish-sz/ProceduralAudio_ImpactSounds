// procaudio_unity: a C API over procaudio-core for Unity (or any C host).
//
// Unity calls it from C#: voices are created on the main thread (built on a
// worker pool, since cold roll/slide tables take ~0.5 s), driven with
// impact / motion commands from the main thread, and rendered from
// OnAudioFilterRead on the audio thread.
//
// Threads:
//  - main thread:   pa_init, pa_voice_create, pa_voice_impact, pa_voice_motion,
//                   pa_voice_release, pa_shutdown
//  - worker pool:   builds voices (make_desc_voice) and publishes them
//  - audio thread:  pa_voice_process (any number of audio threads; one voice
//                   is only ever processed by one caller at a time)
// Commands reach the audio thread through a lock-free ring (impacts) and
// atomics (motion); a voice's mutex only guards process against release.
//
// A voice that has been silent for a while (no motion, output under
// kQuietPa) goes dormant: process() then skips the modal banks entirely, so
// hundreds of idle voices cost nothing.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h>
#define PA_HAVE_SSE 1
#endif

#include "procaudio/body_desc.h"
#include "procaudio/catalog.h"
#include "procaudio/voice.h"

#if defined(_WIN32)
#define PA_API extern "C" __declspec(dllexport)
#else
#define PA_API extern "C" __attribute__((visibility("default")))
#endif

// Mirrors pa::BodyDesc with plain types (C# StructLayout.Sequential).
struct PaDesc {
    int shape;        // pa::Shape: 0 Ball, 1 Stick, 2 Box, 3 Panel, 4 Ground
    int material;     // index into pa::body_material(): metal, glass, wood, plastic,
                      // rubber, stone, cardboard, concrete, ceramic
    float size;       // m (see body_desc.h)
    float thickness;  // m, 0 = auto
    int mount;        // pa::Mount: 0 Free, 1 Held, 2 Resting, 3 BuiltIn
    float hit;        // 0 center .. 1 edge
    int finish;       // pa::Finish: 0 Bare, 1 Polished, 2 Tiled, 3 Carpeted
};

namespace {

constexpr int kMaxVoices = 8192;
constexpr int kRing = 64;
constexpr double kQuietPa = 1e-5;     // output below this counts as silence
constexpr double kQuietTime = 0.3;    // s of silence before a voice sleeps

pa::BodyDesc to_desc(const PaDesc& d) {
    pa::BodyDesc b;
    b.shape = static_cast<pa::Shape>(std::clamp(d.shape, 0, static_cast<int>(pa::Shape::Count) - 1));
    b.material = pa::body_material(d.material);
    b.size = d.size;
    b.thickness = std::max(0.0f, d.thickness);
    b.mount = static_cast<pa::Mount>(std::clamp(d.mount, 0, static_cast<int>(pa::Mount::Count) - 1));
    b.hit = std::clamp(d.hit, 0.0f, 1.0f);
    b.finish = static_cast<pa::Finish>(std::clamp(d.finish, 0, static_cast<int>(pa::Finish::Count) - 1));
    return b;
}

struct Slot {
    std::atomic<int> state{0};   // 0 free/building, 1 ready, -1 failed, -2 released
    std::unique_ptr<pa::ContactVoice> voice;
    std::mutex mtx;              // process vs release
    // impacts: main thread writes head, audio thread writes tail
    float ring[kRing] = {};
    std::atomic<unsigned> head{0}, tail{0};
    std::atomic<int> motion{0};  // 0 none, 1 roll, 2 slide
    std::atomic<float> speed{0.0f};
    // audio-thread state
    bool dormant = true;
    long long quiet = 0;
    std::vector<float> scratch;
};

struct Job {
    int handle;
    pa::BodyDesc obj, tgt;
    bool motion;
};

struct Engine {
    double fs = 48000.0;
    Slot* slots = nullptr;       // kMaxVoices, never moved
    std::atomic<int> next{1};    // slot 0 unused; handles are never reused
    std::mutex qmtx;
    std::condition_variable qcv;
    std::deque<Job> queue;
    std::vector<std::thread> workers;
    std::atomic<bool> stop{false};
    std::atomic<int> pending{0};
    std::mutex err_mtx;
    std::string last_error;
};

Engine* g = nullptr;

// Decaying modal states turn into denormals, which cost ~100x on x86: a
// ringing-out box impact went from 2 % to 280 % of a core. Flush them to
// zero for the duration of a process call (FTZ | DAZ), then restore the
// host's mode.
struct DenormalGuard {
#if PA_HAVE_SSE
    unsigned old = _mm_getcsr();
    DenormalGuard() { _mm_setcsr(old | 0x8040u); }
    ~DenormalGuard() { _mm_setcsr(old); }
#endif
};

Slot* slot_of(int h) {
    if (!g || h <= 0 || h >= kMaxVoices || h >= g->next.load()) return nullptr;
    return &g->slots[h];
}

void worker_loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(g->qmtx);
            g->qcv.wait(lock, [] { return g->stop.load() || !g->queue.empty(); });
            if (g->stop.load()) return;
            job = std::move(g->queue.front());
            g->queue.pop_front();
        }
        Slot& s = g->slots[job.handle];
        if (s.state.load() == -2) { g->pending.fetch_sub(1); continue; }   // released before built
        try {
            auto v = pa::make_desc_voice(job.obj, job.tgt, g->fs, job.motion);
            v->set_smoothing(0.08);
            s.scratch.assign(4096, 0.0f);
            {
                std::lock_guard<std::mutex> lock(s.mtx);
                if (s.state.load() != -2) {
                    s.voice = std::move(v);
                    s.state.store(1, std::memory_order_release);
                }
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(g->err_mtx);
            g->last_error = e.what();
            s.state.store(-1);
        }
        g->pending.fetch_sub(1);
    }
}

}  // namespace

// ------------------------------------------------------------------ engine

PA_API int pa_version() { return 1; }

// Start the engine at the host's output rate. workers: voice-building
// threads (<= 0: 2). Calling it again with the engine running is a no-op.
PA_API int pa_init(int sample_rate, int workers) {
    if (g) {                      // already running (Unity keeps the DLL loaded
        if (sample_rate > 0) g->fs = sample_rate;   // across Play sessions)
        return 1;
    }
    g = new Engine();
    g->fs = sample_rate > 0 ? sample_rate : 48000;
    g->slots = new Slot[kMaxVoices];
    const int n = workers > 0 ? workers : 2;
    for (int i = 0; i < n; ++i) g->workers.emplace_back(worker_loop);
    return 1;
}

PA_API void pa_shutdown() {
    if (!g) return;
    {
        std::lock_guard<std::mutex> lock(g->qmtx);
        g->stop.store(true);
        g->queue.clear();
    }
    g->qcv.notify_all();
    for (auto& t : g->workers) t.join();
    delete[] g->slots;
    delete g;
    g = nullptr;
}

// Drops every voice and queued build, waits for builds in flight, and starts
// handles from 1 again. Safe while the audio thread is still calling
// pa_voice_process (those calls return 0). For hosts that keep the DLL
// loaded between sessions (the Unity Editor): call it when a session ends.
PA_API void pa_reset() {
    if (!g) return;
    {
        std::lock_guard<std::mutex> lock(g->qmtx);
        g->pending.fetch_sub(static_cast<int>(g->queue.size()));
        g->queue.clear();
    }
    while (g->pending.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const int used = std::min(g->next.load(), kMaxVoices);
    for (int h = 1; h < used; ++h) {
        Slot& s = g->slots[h];
        std::lock_guard<std::mutex> lock(s.mtx);
        s.voice.reset();
        std::vector<float>().swap(s.scratch);
        s.head.store(0);
        s.tail.store(0);
        s.motion.store(0);
        s.speed.store(0.0f);
        s.dormant = true;
        s.quiet = 0;
        s.state.store(0);
    }
    g->next.store(1);
}

PA_API int pa_sample_rate() { return g ? static_cast<int>(g->fs) : 0; }
PA_API int pa_pending_builds() { return g ? g->pending.load() : 0; }

// Copies the last build error into buf (UTF-8, NUL-terminated); returns its length.
PA_API int pa_last_error(char* buf, int cap) {
    if (!g || !buf || cap <= 0) return 0;
    std::lock_guard<std::mutex> lock(g->err_mtx);
    const int n = std::min(cap - 1, static_cast<int>(g->last_error.size()));
    std::memcpy(buf, g->last_error.data(), static_cast<size_t>(n));
    buf[n] = 0;
    return n;
}

// ------------------------------------------------------------------ bodies

// Mass (kg) of the body a description builds (for the game's rigidbody).
PA_API float pa_body_mass(const PaDesc* d) {
    if (!d) return 0.0f;
    try {
        return static_cast<float>(pa::make_body(to_desc(*d)).mass);
    } catch (...) {
        return 0.0f;
    }
}

// Thickness (m) the body uses when the description's thickness is 0.
PA_API float pa_default_thickness(const PaDesc* d) {
    if (!d) return 0.0f;
    const pa::BodyDesc b = to_desc(*d);
    const double size = std::min(pa::size_max(b.shape), std::max(pa::size_min(b.shape), b.size));
    return static_cast<float>(b.thickness > 0.0 ? b.thickness
                                                : pa::default_thickness(b.shape, b.material, size));
}

// Lowest mode (Hz) of the body, 0 if it has none.
PA_API float pa_body_f0(const PaDesc* d) {
    if (!d) return 0.0f;
    try {
        pa::Body b = pa::make_body(to_desc(*d));
        double f = 0.0;
        for (const pa::Mode& m : b.modes) f = (f == 0.0) ? m.freq : std::min(f, m.freq);
        return static_cast<float>(f);
    } catch (...) {
        return 0.0f;
    }
}

// ------------------------------------------------------------------ voices

// A voice for `obj` against `tgt`. motion = 0: impacts only (cheap to
// build); 1: also rolling (balls) / sliding tables. Returns a handle (> 0)
// at once; the voice is built on a worker and plays once pa_voice_state()
// reports 1. Commands sent before that are dropped.
PA_API int pa_voice_create(const PaDesc* obj, const PaDesc* tgt, int motion) {
    if (!g || !obj || !tgt) return 0;
    const int h = g->next.fetch_add(1);
    if (h >= kMaxVoices) return 0;
    g->pending.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(g->qmtx);
        g->queue.push_back(Job{h, to_desc(*obj), to_desc(*tgt), motion != 0});
    }
    g->qcv.notify_one();
    return h;
}

// 0 building, 1 ready, -1 build failed (see pa_last_error), -2 released.
PA_API int pa_voice_state(int h) {
    Slot* s = slot_of(h);
    return s ? s->state.load() : -1;
}

// An impact at normal speed v (m/s), played at the start of the next block.
PA_API void pa_voice_impact(int h, float v) {
    Slot* s = slot_of(h);
    if (!s || s->state.load() != 1 || !(v > 0.0f)) return;
    const unsigned head = s->head.load(std::memory_order_relaxed);
    if (head - s->tail.load(std::memory_order_acquire) >= kRing) return;   // full: drop
    s->ring[head % kRing] = std::min(v, 30.0f);
    s->head.store(head + 1, std::memory_order_release);
}

// Continuous motion: 0 none, 1 roll, 2 slide; speed in m/s (smoothed, 80 ms).
// A voice without the asked-for exciter uses the other one (a ball held
// still slides, a box cannot roll).
PA_API void pa_voice_motion(int h, int motion, float speed) {
    Slot* s = slot_of(h);
    if (!s) return;
    s->motion.store(std::clamp(motion, 0, 2), std::memory_order_relaxed);
    s->speed.store(std::isfinite(speed) ? std::clamp(speed, 0.0f, 20.0f) : 0.0f,
                   std::memory_order_relaxed);
}

// Adds n mono samples * gain into out (does not clear it). Returns 1 if the
// voice was awake (made sound), else 0. Audio thread; never blocks on a build.
PA_API int pa_voice_process(int h, float* out, int n, float gain) {
    Slot* s = slot_of(h);
    if (!s || !out || n <= 0 || s->state.load(std::memory_order_acquire) != 1) return 0;
    std::unique_lock<std::mutex> lock(s->mtx, std::try_to_lock);
    if (!lock.owns_lock() || !s->voice) return 0;
    pa::ContactVoice& v = *s->voice;
    DenormalGuard ftz;

    // impacts
    unsigned tail = s->tail.load(std::memory_order_relaxed);
    const unsigned head = s->head.load(std::memory_order_acquire);
    for (; tail != head; ++tail) {
        v.impact(s->ring[tail % kRing], 0);
        s->dormant = false;
        s->quiet = 0;
    }
    s->tail.store(tail, std::memory_order_release);

    // motion
    const int want = s->motion.load(std::memory_order_relaxed);
    const float speed = s->speed.load(std::memory_order_relaxed);
    pa::Motion m = pa::Motion::None;
    if (want == 1) m = v.roll() ? pa::Motion::Roll : (v.slide() ? pa::Motion::Slide : pa::Motion::None);
    if (want == 2) m = v.slide() ? pa::Motion::Slide : (v.roll() ? pa::Motion::Roll : pa::Motion::None);
    if (speed <= 0.0f) m = pa::Motion::None;
    if (m != pa::Motion::None) {
        // the exciter's smoother starts from where it stopped: switch the
        // motion on only while it is wanted, then let the speed glide to 0
        v.set_motion(m);
        s->dormant = false;
        s->quiet = 0;
    }
    v.set_target_speed(m == pa::Motion::None ? 0.0 : speed);
    if (s->dormant) return 0;

    if (static_cast<int>(s->scratch.size()) < n) s->scratch.assign(static_cast<size_t>(n), 0.0f);
    float* buf = s->scratch.data();
    v.process(buf, n, 1.0);
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        float x = buf[i];
        if (!std::isfinite(x)) x = 0.0f;
        peak = std::max(peak, std::fabs(x));
        out[i] += x * gain;
    }
    if (m == pa::Motion::None && peak < kQuietPa) {
        s->quiet += n;
        if (s->quiet > static_cast<long long>(kQuietTime * g->fs)) {
            s->dormant = true;
            v.set_motion(pa::Motion::None);
        }
    } else {
        s->quiet = 0;
    }
    return 1;
}

// Frees the voice. The handle is never reused; later calls with it are no-ops.
PA_API void pa_voice_release(int h) {
    Slot* s = slot_of(h);
    if (!s) return;
    std::lock_guard<std::mutex> lock(s->mtx);
    s->state.store(-2);
    s->voice.reset();
    std::vector<float>().swap(s->scratch);
}
