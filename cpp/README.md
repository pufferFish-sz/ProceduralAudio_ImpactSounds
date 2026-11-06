# procaudio-core (C++) and the FMOD plugin

A C++17 port of the Python prototype in `proto/`, with no dependencies:
- impact (implicit Hunt–Crossley);
- rolling (`core/rolling.py`);
- sliding model C (`core/slide_profile.py`, `scenes/drag.py`).

It also has an FMOD sound-module adapter. The design is in
[`proto/docs/cpp_plugin_design.md`](../proto/docs/cpp_plugin_design.md).
The Python code stays the reference; `tests/golden.py` checks this port
against it.

## Layout

```
core/include/procaudio/
  core.h      constants, PCG32 RNG, materials, Hertz helpers, ModalBank
  dsp.h       FFT, periodic cubic spline, decimator, biquad, ClickFilter, ControlSmoother
  profile.h   surface presets, periodic surface tiles, circle dilation, rim profiles
  bodies.h    shapes (bar, plate, Lamb sphere), radiation (compact, Maidanik),
              floor / ball / box factories, SoundingBody
  exciters.h  simulate_impact_implicit, RollExciter, SlideExciter (streaming)
  voice.h     ContactVoice: object + floor + exciters + scheduled impacts
  scenes.h    offline scenes mirroring the Python listening sets, WAV output
tools/render.cpp          procaudio_render: scenes + golden-case runner
tests/test_core.cpp       unit + real-time behaviour tests
tests/golden.py           C++ vs Python golden tests
plugins/fmod/             FMOD sound module (built only if FMOD_SDK_DIR is set)
```

## Build and test (Windows, Visual Studio 2022, CMake ≥ 3.20)

```powershell
cmake -S cpp -B cpp/build -G "Visual Studio 17 2022" -A x64
cmake --build cpp/build --config Release
cpp/build/Release/procaudio_tests.exe
& "$env:USERPROFILE\miniconda3\envs\procaudio\python.exe" cpp/tests/golden.py
```

Listening renders from the C++ core:

```powershell
cpp/build/Release/procaudio_render.exe scene roll_spheres cpp/build/renders/roll_spheres 48000
cpp/build/Release/procaudio_render.exe scene drag_boxes   cpp/build/renders/drag_boxes   48000
cpp/build/Release/procaudio_render.exe scene ball_drop    cpp/build/renders/ball_drop    48000
```

## Status (2026-10-28)

- **`procaudio_tests`: 18/18 pass.**
  - FFT, spline, decimator and modal decay accuracy.
  - Click-filter magnitude within 0.5 dB of the exact radiation law.
  - Impulse conservation in the impact solver.
  - Bit-identical output for 64- and 512-sample host blocks.
  - No hard edges in roll or slide output: largest sample step 5.8 and
    0.3 dB over the rms.
- **`golden.py`: 18/18 pass.**

  | path | result vs Python |
  |---|---|
  | modal bank | max error 7e-13 |
  | impact | force within 1e-12; contact time and rebound identical |
  | rolling, same table | force correlation 0.9998 (large ball), 0.995 (small ball, hopping), identical airborne fraction and landing counts |
  | sliding, same table | correlation 0.991–1.000 |
  | scene renders | LTAS shape distance 0.2–5.7 dB, levels within 1.1 dB |

- **The golden test found a bug in the Python reference**
  (`scenes/drag.steady_pull`). The hand-speed variation came out ±1.2 %
  instead of the intended ±12 %, because `sosfiltfilt`'s start-up transient
  dominated the normalization. It is fixed, and `renders/scenes/drag_boxes`
  was re-rendered.
- **Speed: single core, scalar code, 48 kHz, table builds included.**

  | scene | speed | CPU per voice |
  |---|---|---|
  | ball drop | 17× real time | ~6 % of a core |
  | rolling | 12× | ~8 % |
  | box drag | 6× | ~16 % |

  Most of this is the per-voice 479-mode floor bank and model C's 24
  track-steps per sample. The design's next steps cut it: a shared,
  budget-culled floor and SIMD (design doc §6).

## FMOD plugin

`plugins/fmod/procaudio_fmod.cpp` is a sound module (0 inputs, mono out).
It is written against the FMOD 2.02 plug-in API, to match the installed
FMOD Studio 2.02.23.

**Parameters (v1.2).** `procaudio_fmod.plugin.js` lays them out in Studio:
150 px dropdowns for Object, Surface and Motion, dials for the rest. The
deck's context menu offers Level presets per motion.

| parameter | type | meaning |
|---|---|---|
| Object | dropdown | wood / steel ball S/L, cardboard / steel box S/L |
| Surface | dropdown | varnished wood, wood floor, concrete, tile, rug |
| Motion | dropdown | **Impact**: one hit when the instrument starts. **Drop**: a dropped object (balls bounce; boxes land, rock on their edges and settle, Housner rocking). **Roll** / **Slide**: continuous at Speed, never a hit. |
| Speed | dial, m/s | roll/slide speed, 80 ms smoothing (default 0.5) |
| Impact | dial, m/s | hit / landing speed (default 2). In Impact and Drop it fires at the instrument start and on every new value. |
| Level | dial, dB | gain over the calibration 0 dBFS = 63 Pa at 1 m (130 dB SPL), range −40…+80. Presets: Impact / Drop 0, Roll +35, Slide +50. |
| Contacts | data | `ContactMsg[]` for game code: kind 0 impact, 1 slide, 2 roll, 3 release, 4 drop; sample offsets |

**Start-up.** Studio creates a fresh plug-in instance on every Play.
- **v1.0:** built its surface tables and impact calibration each time, so
  every Play stayed silent for 0.5–0.95 s.
- **v1.1:** takes everything from a process-wide cache (`core/catalog.h`).
  - A warm voice is ready in ~5–9 ms. Sound starts in the first mix block.
  - The cache is prewarmed on a background thread when FMOD registers the
    plug-in: all 8 × 5 presets in ~9 s.
  - Only a Play within those first seconds can still wait, up to ~0.7 s.

**Built and tested (2026-11-06, v1.2).** Built against the FMOD Engine
2.02.23 SDK. The DLL exports `FMODGetDSPDescription`.

`plugins/fmod/fmod_host_test.cpp` loads it into the real FMOD engine and
drives it the way Studio does: a new instance per Play, parameters set right
after creation. It renders through FMOD's NRT WAV writer.
`tests/fmod_host_check.py` checks the render. All pass:

| segment | result |
|---|---|
| impact, steel ball L | sounds in the first mix block; a one-shot (tail −42 dB) |
| impact retrigger | a new Impact value fires again |
| drop, cardboard box | landing + rocking settle; one-shot |
| drop, steel ball | bounce series; one-shot |
| roll, Impact set to 2 | Impact does not fire; onset +1.6 dB re steady state (no hit) |
| slide | onset −1.0 dB (no hit) |
| after stop | digital silence |

**Build:**

```powershell
cmake -S cpp -B cpp/build "-DFMOD_SDK_DIR=C:/Program Files (x86)/FMOD SoundSystem/FMOD Studio API Windows"
cmake --build cpp/build --config Release
& "$env:USERPROFILE\miniconda3\envs\procaudio\python.exe" cpp/tests/fmod_host_check.py
```

The output is `cpp/build/plugins/fmod/Release/procaudio_fmod.dll`.

**Install:**
- **FMOD Studio 2.02:** copy `procaudio_fmod.dll` and
  `procaudio_fmod.plugin.js` (both in the build output folder) into
  `%localappdata%\FMOD Studio\Plugins`, then restart Studio. That folder is
  loaded automatically; the custom-folder setting is under Preferences →
  Assets. For a shared project, use a `Plugins` folder next to the `.fspro`
  instead.
- **Using it:** right-click an audio track → Add Instrument → Plug-in
  Instrument → ProcAudio Contact. If it is missing, check Window → Console
  → Logging.
- **Unity (FMOD for Unity 2.02):** copy the DLL to
  `Assets/Plugins/FMOD/platforms/win/lib/x86_64/`. In its import settings,
  tick Windows x86_64 only. Add `procaudio_fmod` to FMOD → Edit Settings →
  Dynamic Plugins.

**Known limitations, to address before shipping:**
- `ContactVoice::impact()` allocates while solving the contact. It is
  bounded, but not yet allocation-free on the mixer thread.
- Each instance owns its floor bank; the shared `SurfaceVoice` is not done
  yet.
- FMOD 2.02 has no `FINITE_LENGTH` data parameter (that is 2.03), so
  instances are open-ended. The plugin reports silence after 3 s idle.
- **Levels.** The radiation fix of 2026-11-06 is described in
  `proto/docs/rolling_sliding_vs_recordings.md` §12. Physical levels at 1 m:

  | event | level |
  |---|---|
  | 2 m/s steel ball L impact | ~56 Pa peak (129 dB SPL) |
  | rolling | 51–76 dB SPL rms |
  | sliding | 51–64 dB SPL rms |

  No fixed calibration fits all of that. 0 dBFS = 63 Pa makes the loudest
  default impact just fit. Raise Level for roll / slide events; the deck
  presets do that.
