# C++ plugin design: impact, sliding and rolling

Working document (2026-10-28). It covers how the Python prototype
(impact: `core/impact.py`, `core/bodies.py`; sliding model C:
`core/slide_profile.py`, `scenes/drag.py`; rolling: `core/rolling.py`)
becomes one engine-agnostic C++ library with thin FMOD and Wwise adapters.
It refines `plan_and_brainstorm.md` §3 with the models as they stand after
`rolling_sliding_vs_recordings.md` §1–11.

The SDK facts below were checked against the FMOD 2.03 and Wwise 2026.1
documentation (links at the end).

---

> **Status (2026-10-28):** milestones 1–2 and most of 3 are done in
> [`cpp/`](../../cpp/README.md).
> - **Done:** the core library (impact, roll, slide, `ContactVoice`), the
>   offline renderer, C++ tests (18/18), and golden tests against this
>   prototype (18/18).
> - **Built (2026-11-05):** the FMOD 2.02 sound module, against the FMOD
>   Engine 2.02.23 SDK. Integration-tested inside the real FMOD engine
>   (`cpp/tests/fmod_host_check.py`).
> - **Open:** the shared `SurfaceVoice` and SIMD.

## 1. Architecture

```
Game engine (Unity / Unreal)
  ContactSensor: per object pair -> impact events + slide/roll control (50-60 Hz)
        │  timestamped structs (FMOD data parameter / Wwise plugin custom data)
        ▼
FMOD sound module  |  Wwise source plugin      <- thin adapters: params, threading, buffers
        ▼
procaudio-core (C++17, no dependencies)
  ObjectVoice  = object modal bank + exciters (Impact | Slide | Roll) + click radiator
  SurfaceVoice = floor modal bank, SHARED by every contact on that surface
  AssetCache   = surface profile tables, mode sets, lookup tables (built off the audio thread)
```

**One voice per object, not per hit.**
- It keeps the object's ringing state. A ball that bounces on a still-ringing
  floor works as it does in `bodies.py` today.
- The voice can switch between impact, slide and roll without restarting.

**One floor per surface, not per object.**
- The floor is the most expensive part: 479 modes up to 10 kHz in the
  wood-floor preset.
- The Python scenes give every scene its own floor. In real time, all
  contacts on the same surface feed their forces into one shared
  `SurfaceVoice`.
- The modal coupling then uses one representative contact point. The
  floor's response to different contact points is not resolved.

## 2. Mapping from Python to C++

| Python | C++ runtime | Precomputed, off the audio thread |
|---|---|---|
| `modal_bank` | `ModalBank`: complex one-pole per mode, SoA, SIMD; outputs velocity directly (no `np.diff`) | coefficients whenever Size/Material change |
| `shapes`, `materials` | — | closed-form mode sets; the sphere's Lamb roots as a table |
| `impact._simulate_implicit` | `ImpactExciter`: implicit Hunt–Crossley step, Newton with a bisection fallback, capped iterations | G and Gd per mode set |
| `bodies.calibrate_mu` | lookup only | μ table per (object material, surface, speed) |
| `slide_profile` (model C) | `SlideExciter`: 6 tracks × 4× oversampling, cubic-spline profile read, lift-off, Hertz landing pulses, 0.3 ms force rebuild after landing (`T_ON`) | profile tables per surface |
| `rolling` | `RollExciter`: one vertical degree of freedom at 4–16× oversampling, Kelvin–Voigt with the 0.3 ms touchdown ramp (`t_on`), f ≥ 0 | dilated profile per (surface, radius bucket); rim table per object |
| `radiation` per-mode σ | folded into per-mode output gains | σ(fₙ) per mode |
| `radiation.accel_to_pressure` (FFT) | `ClickRadiator`: causal 2nd-order filter approximating the compact-dipole response | filter fit per object size |
| `friction` (model A), `aggregate` | `NoiseExciter`, `GrainExciter` (biquads, fixed grain pool) | — |
| scene shared gain, `Scene.trim_db` | calibration constant (Pa → dBFS) plus a designer `Trim` parameter | — |

## 3. Parts that need redesign rather than a straight port

1. **Profile tables.** Python builds one table per render with FFTs, circle
   dilation and Gaussian smoothing. In C++:
   - Build a looping tile of about 2–4 m per surface on a worker thread. At
     the 20 µm grid that is about 200 KB per metre in float.
   - Keep dilated versions per object-radius bucket, e.g. ½-octave steps.
   - Cache all of this in `AssetCache`.
   - The audio thread only does cubic reads at x(t). The cubic read matters:
     linear interpolation of the table caused a v/DX line and aliasing arcs
     (`rolling_sliding_vs_recordings.md` §3).
2. **Click radiation.** Python filters the whole file with an FFT, which is
   offline-only. Replace it with a causal IIR. A compact body radiates
   p ∝ dF/dt, so a 2nd-order high-pass tuned to ka is the natural form.
   Validate it against the FFT version on the existing scenes.
3. **Oversampling.**
   - The exciter contact loops run at 4–16× and are decimated with one
     polyphase FIR. The modal banks stay at the host rate.
   - Rolling can raise the factor only near contact switches (flights and
     landings) and run steady rolling at 4×.
   - Contact resonances above fs_s/20 are softened as in `roll_force`.
4. **Sample-rate independence.** Python runs at 44.1 kHz; FMOD and Wwise
   usually run at 48 kHz. Everything takes `fs` from the host: the 0.3 ms
   ramps, the stiffness cap and the decimation filters.
5. **Real-time rules:**
   - no allocation or locks in the process callback (Wwise requires
     `AK_PLUGIN_ALLOC`);
   - fixed maxima for modes, tracks and grains;
   - flush-to-zero for denormals;
   - a seeded per-voice RNG;
   - output identical regardless of block size.
6. **Parameter changes.** Size, Material and Archetype change the mode sets
   and profiles. Recompute on a worker or host thread, then hand over to the
   audio thread by atomic pointer swap. The FMOD setters run on the user
   thread and are documented as not thread-safe against `process`.
7. **Smooth controls.** Every speed and force control is smoothed to audio
   rate with zero acceleration at starts and stops. Speed steps caused the
   "start bump" (`rolling_sliding_vs_recordings.md` §8 and §11).

## 4. What the game sends

The engine sorts each contact pair into impact, slide or roll. That is
cheap, and the engine has the data:

- **Impact:** a new contact with normal speed above a threshold. Send
  `v_n`, the effective mass, the contact point and a timestamp.
  - Unity: `OnCollisionEnter` (`relativeVelocity`, `impulse`). The batch
    API `Physics.ContactEvent` gives impulse, normal and position but no
    velocity; get that from `Rigidbody.GetPointVelocity`.
  - Unreal: `OnComponentHit` (`NormalImpulse`, which needs "Simulation
    Generates Hit Events" and is zero for swept moves).
- **Roll vs slide:** take the slip speed, v_slip = |v_t − ω × r| at the
  contact.
  - small slip with |ω × r| > threshold → **roll**, with speed |ω × r|;
  - otherwise → **slide**, with speed |v_t|.
- **Every physics tick:** send the normal force (or the weight), the
  contact state and the surface ID. The engine's physical-material tag maps
  to a surface asset.

The plugin smooths these 50–60 Hz controls to audio rate (§3.7). Impacts
arrive as events with sample-accurate offsets inside the data payload,
because neither middleware's parameters are sample-accurate.

Suggested payload (one struct per contact change, packed into an array):

```cpp
struct ContactMsg {
    uint32_t object_id, surface_id;
    uint8_t  kind;           // 0 impact, 1 slide, 2 roll, 3 release
    float    t_offset_s;     // time inside the next audio block
    float    v_normal;       // impact: normal speed (m/s)
    float    v_tangent;      // slide: slip speed; roll: |omega x r| (m/s)
    float    normal_force;   // N (weight while resting / sliding / rolling)
    float    eff_mass;       // kg (impact)
};
```

## 5. Middleware wrappers

**FMOD (2.03)**
- The plugin is a *sound module*: an `FMOD_DSP_DESCRIPTION` with
  `numinputbuffers = 0`, placed on an event track. Studio builds the UI
  from the parameter descriptors.
- **Designer controls:**
  - float parameters for Size, Hollow, Ring, Brightness and Trim (dB);
  - int dropdowns for Material and Archetype.
- **Game data:** a `FMOD_DSP_PARAMETER_DATA_TYPE_USER` data parameter
  carrying `ContactMsg[]`.
- **Plugin handle:** game code gets it from
  `FMOD_STUDIO_EVENT_CALLBACK_PLUGIN_CREATED`.
- **Lifetime:** expose `FMOD_DSP_PARAMETER_DATA_TYPE_FINITE_LENGTH`, and
  return `FMOD_ERR_DSP_SILENCE` once the voice energy drops below a
  threshold.
- **Build:** a DLL for Studio exporting `FMODGetDSPDescription` (or
  `FMODGetPluginDescriptionList`). In Unity: the Dynamic Plugins list, or
  static plugins on IL2CPP.

**Wwise (2026.1)**
- **Scaffolding:** `wp.py new` (source plugin), then `premake`, `build`,
  `package`.
- **Runtime:** `AK::IAkSourcePlugin` (`Init`, `Execute`, `GetDuration` = 0
  for open-ended voices, `TimeSkip`).
- **Parameters:** RTPC-capable properties via XML.
- **Game data:** `AK::SoundEngine::SendPluginCustomGameData`, read with
  `IAkSourcePluginContext::GetPluginCustomGameData()`. Forum reports of
  trouble on source plugins mean this path should be prototyped early.
  Fallback: an exported C function in the plugin DLL that the game calls
  directly.
- **Shipping:** a DLL in `Wwise/Deployment/Plugins/[Platform]/DSP` for
  Unity; static libraries for Unreal. Request a reserved company ID from
  Audiokinetic before distributing.

**Unreal alternative:** a custom MetaSound node on the same core may be
less work than the Wwise integration.

## 6. Cost

Rough cost per object voice per sample at the host rate:

| part | flops / sample |
|---|---|
| 10–20 object modes | ~100 |
| slide, 6 tracks × 4× | ~700 |
| roll, up to 16× | ~300 |
| decimation FIR | ~130 |

That is about 1,000–1,300 flops per sample, or 50–65 MFLOP/s at 48 kHz:
roughly 1–2 % of a core in scalar code and well under 1 % with SIMD. Slide
and roll rarely run at the same time on one voice.

The shared `SurfaceVoice` (480 modes → about 128 after budget culling,
~800 flops/sample) is paid once per surface, not per object.

Measure rather than trust these numbers: a benchmark harness should sweep
modes × voices × block size before any engine work.

## 7. Validation

- **Golden tests:** wrap the core with pybind11. Export Python's profile
  tables and noise arrays as binary files so C++ and Python see the same
  input, then compare:
  - impact contact time and peak force;
  - landing counts and airborne fraction;
  - sample error on the modal outputs.
- **Statistics:** reuse `analysis/texture_stats.py` on C++ renders. Shape
  distance and the texture measures should match the Python renders, which
  in turn are tracked against the recordings.
- **Real-time behaviour:**
  - block-size invariance (64–1024 samples, identical output);
  - 50 Hz control vs audio rate (no zipper noise);
  - no waveform steps: re-run the max-sample-step check of
    `rolling_sliding_vs_recordings.md` §10–11 on C++ output;
  - voice stealing in a pile-collapse scene.

## 8. Order of work

| # | milestone |
|---|---|
| 1 | Core skeleton: `ModalBank`, `ImpactExciter`, `ClickRadiator`, an offline WAV-rendering command-line tool; golden tests vs Python |
| 2 | `RollExciter` and `SlideExciter` with `AssetCache` (worker-thread tables) |
| 3 | FMOD sound module; first used in Studio, then in Unity |
| 4 | Unity `ContactSensor` with impact/slide/roll classification and the `ContactMsg` channel |
| 5 | Wwise source plugin on the same core (Unity first, then Unreal) |
| 6 | In-engine CPU measurements and the perceptual evaluation (`evaluation_protocol.md`) |

## Open questions

- **Shared floor:** is a single representative contact point good enough,
  or does each contact zone need its own floor bank (e.g. one per room
  region)?
- **Tangential friction:** model it before the port. It is the likely
  missing source of the metal boxes' 4–16 kHz content and of cardboard's
  real loudness (`rolling_sliding_vs_recordings.md` §9–11).
- **Primary middleware:** FMOD or Wwise for the thesis demo (licensing,
  what the program uses).

## References

- FMOD Plug-in API Reference, DSP (2.03): https://www.fmod.com/docs/2.03/api/plugin-api-dsp.html
- FMOD Plug-in DSP API Guide (2.03): https://www.fmod.com/docs/2.03/api/dsp-plugin-api-guide.html
- FMOD Studio API, EventInstance (2.03): https://www.fmod.com/docs/2.03/api/studio-api-eventinstance.html
- FMOD for Unity, Plugins (2.03): https://fmod.com/docs/2.03/unity/plugins.html
- Wwise SDK 2024.1, Using the Development Tools: https://www.audiokinetic.com/en/public-library/2024.1.18_9194/?source=SDK&id=effectplugin_tools.html
- Wwise SDK 2026.1, Creating Sound Engine Plug-ins: https://www.audiokinetic.com/en/public-library/2026.1.3_9276/?source=SDK&id=soundengine_plugins.html
- Wwise SDK 2026.1, Creating Sound Engine Source Plug-ins: https://www.audiokinetic.com/en/public-library/2026.1.3_9276/?source=SDK&id=soundengine_plugins_source.html
- Audiokinetic Q&A, custom game data for source plugins: https://audiokinetic.com/qa/10718/custom-plugin-data-for-source-dsps
- Unity `Physics.ContactEvent`: https://docs.unity3d.com/ScriptReference/Physics.ContactEvent.html
- Unity `ContactPairPoint`: https://docs.unity3d.com/ScriptReference/ContactPairPoint.html
- Unreal `OnComponentHit`: https://dev.epicgames.com/documentation/en-us/unreal-engine/BlueprintAPI/Collision/OnComponentHit
