# Procedural Contact Audio for Games

Real-time procedural sound for physical contact in games and extended reality:
impacts, drops, rolling and sliding. The sound is synthesized from the physics
event itself (which bodies touch, what they are made of, how hard or fast they
move) instead of being picked from a library of recorded samples.

A sound designer describes each body in everyday terms (shape, material, size,
thickness, how it is held, surface finish). The game engine reports each
contact and its speed. Everything else (vibration modes, contact hardness,
rebound, relative loudness) is derived from those descriptions, so any object
can meet any other object or surface without authoring the combinations.

This is a master's thesis project at Carnegie Mellon University (2026–27).

## Demo video

[![Procedural contact audio: Unity demo (watch on YouTube)](https://img.youtube.com/vi/yO-YJDNprkA/hqdefault.jpg)](https://youtu.be/yO-YJDNprkA)

The [Unity demo](Unity%20Demo/README.md) in action: impacts, drops, rolling and
sliding, all synthesized live from the game's physics through the FMOD plug-in.
Click the image to watch on YouTube.

## What is in this repository

| Part | What it is | Details |
|---|---|---|
| [`proto/`](proto/README.md) | Python prototype: the reference implementation of every model, with numerical checks, listening scenes, figures and design notes | [proto/README.md](proto/README.md) |
| [`cpp/`](cpp/README.md) | `procaudio-core`, a dependency-free C++17 library (impact, rolling, sliding), an offline renderer, and tests against the Python reference | [cpp/README.md](cpp/README.md) |
| [`cpp/plugins/fmod/`](cpp/plugins/fmod) | **ProcAudio Contact**, an FMOD Studio sound-module plug-in with an object / target / motion deck and a Studio script menu | [cpp/README.md § FMOD plugin](cpp/README.md#fmod-plugin) |
| [`Unity Demo/`](Unity%20Demo/README.md) | A first-person Unity scene where game physics drives the plug-in through FMOD events | [Unity Demo/README.md](Unity%20Demo/README.md) |
| [`FMOD Test/`](FMOD%20Test) | The FMOD Studio project used by the demo, and the script that generates its events | [Unity Demo/README.md § FMOD side](Unity%20Demo/README.md#fmod-side) |

## How it works

Each contact sound is split into an **exciter** (the contact force) and a
**resonator** (the vibrating bodies):

```
 game engine            exciter (one per contact)           both bodies              radiation
 impact  speed ──►  Hunt–Crossley contact (Hertz k) ─┐                       ┌──► radiation efficiency ─┐
 rolling speed ──►  ball path over surface profile  ─┼─► contact force f(t) ─┤      (vibration → Pa)    ├─► Σ ─► p(t)
 sliding speed ──►  lift-off and landings on profile ┘    on both bodies     └──► modal banks + click ──┘
                         ▲                                                    │
                         └──── impact only: displacement of both bodies ◄─────┘   (coupled solution)
```

- **Bodies.** Modes come from closed-form shapes (beam, plate, Lamb sphere).
  A material sets both the decay of the modes (one internal-friction value,
  tan φ) and their pitch and contact hardness (E, ρ, ν).
- **Two-body contact.** Every contact is two bodies sharing one force; a
  floor is just another body. Hardness (Hertz), effective mass and
  restitution are calculated from the two descriptions, so *N* bodies need
  *N* descriptions, not an *N* × *N* table.
- **Physical levels.** A radiation model converts each body's vibration to
  sound pressure at 1 m, so the balance between object and floor, and
  between impacts, rolling and sliding, comes from the physics rather than a
  mix.

The formulas, with their sources, are in
[`proto/docs/formulas_explained.md`](proto/docs/formulas_explained.md).

## Getting started

Requirements (Windows): Visual Studio 2022, CMake ≥ 3.20, a Python 3.11
conda environment, FMOD Studio and the FMOD Engine SDK 2.02.23, and Unity
6000.3 with FMOD for Unity 2.02.

**1. Python prototype** (from the repository root):

```powershell
conda create -n procaudio -y python=3.11 numpy scipy matplotlib
& $env:USERPROFILE\miniconda3\envs\procaudio\python.exe -m pip install soundfile
$py = "$env:USERPROFILE\miniconda3\envs\procaudio\python.exe"
& $py -m proto.scenes.ball_drop      # renders WAVs to proto/renders/scenes/
```

More stages and scenes: [proto/README.md](proto/README.md).

**2. C++ core and tests:**

```powershell
cmake -S cpp -B cpp/build -G "Visual Studio 17 2022" -A x64
cmake --build cpp/build --config Release
cpp/build/Release/procaudio_tests.exe
& $py cpp/tests/golden.py            # C++ against the Python reference
```

**3. FMOD plug-in:** configure with the SDK path, build, then copy the
plug-in files into FMOD Studio's plug-in folder (steps in
[cpp/README.md § FMOD plugin](cpp/README.md#fmod-plugin)):

```powershell
cmake -S cpp -B cpp/build "-DFMOD_SDK_DIR=C:/Program Files (x86)/FMOD SoundSystem/FMOD Studio API Windows"
cmake --build cpp/build --config Release
```

**4. Unity demo:** open `Unity Demo/ProceduralAudioDemo` in Unity 6000.3,
open `Assets/Scenes/ImpactDemo.unity` and press Play. Pick up, throw, roll
and drag objects, and switch between seven floor surfaces with keys 1–7.
Controls and the FMOD setup are in [Unity Demo/README.md](Unity%20Demo/README.md).

## Status (2026-11)

| Part | State |
|---|---|
| Python prototype | Stage gates 1–5 pass; listening scenes for drops, strikes, tumbling, rolling, sliding, footsteps and grass |
| C++ core | Impact, rolling and sliding ported; unit and real-time tests and golden tests against Python pass ([details](cpp/README.md#status-2026-10-28)) |
| FMOD plug-in | v2.5: any object against any target, driven from game parameters; tested inside the FMOD engine |
| Unity demo | Physics-driven impacts, rolls and slides through FMOD events, on seven floor surfaces |

Not done yet:
- comparison with calibrated recordings, and a listening test;
- granular surfaces (gravel, dirt, grass) in C++ and FMOD (Python only);
- a floor voice shared by all contacts on one surface, and SIMD, to lower the
  cost per voice;
- Wwise and Unreal Engine integration.

## Repository layout

```
proto/          Python prototype (reference implementation)
  core/         models: modal bank, materials, shapes, impact, contact, rolling,
                sliding (slide_profile), radiation, bodies, aggregate, grass
  stages/       numerical stage gates 1–5 (print PASS/FAIL)
  scenes/       listening scenes from fully specified bodies
  analysis/     decay fitting, spectra, texture statistics
  docs/         design notes and formulas (see below)
  recordings/   reference recordings used for comparison
  renders/      rendered WAVs        figures/  PNG figures
cpp/
  core/         procaudio-core library (include/procaudio, src)
  plugins/fmod/ ProcAudio Contact FMOD plug-in, Studio deck and menu scripts
  tools/        procaudio_render: offline scenes and golden-case runner
  tests/        C++ unit tests, golden tests, FMOD host checks
FMOD Test/      FMOD Studio project for the demo, event-generator script
Unity Demo/     Unity 6 demo project (ProcAudio runtime scripts, scene builder)
```

## Design notes

In [`proto/docs/`](proto/docs):

| Document | Contents |
|---|---|
| [formulas_explained.md](proto/docs/formulas_explained.md) | every formula per stage, with its source |
| [plugin_body_design.md](proto/docs/plugin_body_design.md) | any body from shape, material, size and mount; pair physics; FMOD deck; motion rules |
| [cpp_plugin_design.md](proto/docs/cpp_plugin_design.md) | C++ core architecture, real-time rules, what the game sends |
| [rolling_sliding_vs_recordings.md](proto/docs/rolling_sliding_vs_recordings.md) | rolling and sliding models compared with recordings; the radiation model |
| [sliding_sound_literature.md](proto/docs/sliding_sound_literature.md) | sliding and scraping models in the literature |
| [two_body_impact_related_work.md](proto/docs/two_body_impact_related_work.md) | prior work on collisions between two ringing bodies |
| [archetypes.md](proto/docs/archetypes.md) | material table and mode formulas for common objects |
| [evaluation_protocol.md](proto/docs/evaluation_protocol.md) | planned perceptual evaluation |
| [plan_and_brainstorm.md](proto/docs/plan_and_brainstorm.md) | roadmap and open design questions |
