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
  body_desc.h any body from Shape + Material + Size + Mount; pair physics
              (effective mass, restitution, pair_mu); make_pair_voice
              (design: proto/docs/plugin_body_design.md)
  scenes.h    offline scenes mirroring the Python listening sets, WAV output
tools/render.cpp          procaudio_render: scenes + golden-case runner
tests/test_core.cpp       unit + real-time behaviour tests
tests/golden.py           C++ vs Python golden tests
plugins/fmod/             FMOD sound module (built only if FMOD_SDK_DIR is set)
plugins/unity/            procaudio_unity.dll: C API for Unity (copied into
                          Unity Demo/ProceduralAudioDemo/Assets/Plugins/x86_64)
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

**Parameters (v2.4): any object against any target.** The design is in
[`proto/docs/plugin_body_design.md`](../proto/docs/plugin_body_design.md).
`procaudio_fmod.plugin.js` lays the deck out in three framed columns,
OBJECT | TARGET | MOTION, with a line between object and target
(`procaudio_fmod_divider.png`). The deck's context menu offers Level and
Speed presets per motion and four example setups (stick knocks a wooden
ball, two glass marbles clink, cardboard box dropped on concrete, steel
ball rolls on a polished wood floor).

| parameter | type | meaning |
|---|---|---|
| Obj Shape | dropdown | Ball, Stick, Box, Panel, Ground |
| Target | dropdown | **Same as object** (two equal bodies; the other target controls are then ignored, and the target dials read `same as object`), Ball, Stick, Box, Panel, **Ground** (a slab on the earth: concrete, stone) |
| Obj Material / Tgt Material | dropdown | metal, glass, wood, plastic, rubber, stone, cardboard, concrete, ceramic |
| Tgt Finish ("Floor finish", target only) | dropdown | applies to Panel and Ground targets only (other shapes are always bare): the floor surface the object hits, lands, rolls or slides on. **Bare**: the material itself. **Polished**: smooth coat (varnish, lacquer); same hits, smoother roll/slide. **Tiled**: hard ceramic layer; brighter hits, the body underneath still rings. **Carpeted**: soft layer; dull thuds, little bounce, fuzzy rolling. |
| Obj Mount / Tgt Mount | dropdown | **Free** (thrown, falling), **Held** (in a hand; a held stick is a rigid striker), **Resting** (sits on a support), **Built-in** (floor, wall) |
| Obj Size / Tgt Size | log dial, m | ball diameter, stick length, box side, panel/slab span. The label shows what it means: `6.0 cm, 882 g, 7.5 kHz` (size used, mass, lowest mode). |
| Obj Thickness / Tgt Thickness | log dial, m | stick section, box wall, plate thickness; 0 = auto (label: `auto (2.4 cm)`) |
| Obj Hit Pos / Tgt Hit Pos | dial 0..1 ("Hit position") | where on the body the contact is: 0 center (stick: middle), 1 edge (stick: end; held stick: tip). The center of a plate only rings its symmetric modes; toward the edge more modes join in (brighter, more complex). Sticks: which overtones ring, and how heavy the hit feels (a free stick's end feels like a quarter of its mass). No effect on balls (label: `any spot (ball)`). |
| Motion | dropdown | **Impact**: one hit when the instrument starts. **Drop**: the object dropped on the target (balls bounce; boxes land, rock on their edges and settle). **Roll** (Ball only; other shapes slide) / **Slide**: continuous at Speed, never a hit. |
| Speed | log dial, m/s | one knob whose meaning follows Motion (the label says which): **Impact / Drop**: hit or landing speed (`hit at 2 m/s`, `drop 2 m/s = from 20 cm`); it fires at the instrument start and on every new value. **Roll / Slide**: rolling or sliding speed, 80 ms smoothing. Default 2; the Roll / Slide presets set 0.5. |
| Render | dropdown | Both, Object only, Target only: when the other body has its own event, each event plays only its own body |
| Level | dial, dB | gain over the calibration 0 dBFS = 63 Pa at 1 m (130 dB SPL), range −40…+80. Presets: Impact / Drop 0, Roll +35, Slide +50. |
| Contacts | data | `ContactMsg[]` for game code: kind 0 impact, 1 slide, 2 roll, 3 release, 4 drop; sample offsets. The target id field is reserved for the body registry (phase 4). |
| Fit Length (v2.4, "Fit length") | log dial, s | 0 = off: the sound rings as long as it rings. Otherwise the plug-in fades its own output to true silence, sample by sample, ending 1 ms before this time after the start (raised-cosine fade of 30% of the length, 5–100 ms; label `ends at 0.15 s (45 ms fade)`). **ProcAudio › Ending: Fit** writes the instrument's length here. It comes after Contacts, so the other indices stay what game code uses. |

The defaults (Ball / metal / 6 cm on a built-in, varnished 2.4 m wood panel)
are v1.2's "Steel ball L" on "Varnished wood". **Events made with an
earlier version need their plug-in instrument set up again**: Studio
identifies parameters by name, and v1.2's Object / Surface, v2.0's Obj
Finish / Obj Hit / Impact and v2.1's Tgt Shape are gone (v2.2's Target
dropdown now holds 0 = Same as object, then the shapes).

Studio 2.02 decks are static: a deck widget has no enabled or visible
state and no change callback, so controls cannot grey out when another
choice makes them irrelevant. **The plug-in's value labels above
(`same as object`, `6.0 cm, 882 g, 7.5 kHz`, `a held ball can't turn`)
are not shown in Studio's deck**: checked in the Studio window, the deck
dials print their own numbers (`0.06 m`, `2.00 m/s`). FMOD hosts that ask
for value strings get them (the host test reads them). The adaptive view
is the ProcAudio panel (below, design doc Part 10).

**Changing a body never cuts the sound.** The new voice takes over at the
next contact while the old one rings out (a roll/slide crossfades through
the speed smoothers). The host test renders a ringing hit with and without
a Size change in the middle: the two renders are identical (difference
−573 dB).

**Start-up.** Studio creates a fresh plug-in instance on every Play.
- **v1.0:** built its surface tables and impact calibration each time, so
  every Play stayed silent for 0.5–0.95 s.
- **v1.1:** takes everything from a process-wide cache (`core/catalog.h`).
  A warm voice is ready in ~5–9 ms, and sound starts in the first mix block.
- **v2.0:** the cache works per body description:
  - contact damping per pair;
  - rolling tables per ball material × radius step (1/6 octave) × texture;
  - sliding tables per texture.

  Roll and slide tables are built only when Motion needs them. A cold
  impact pair takes a few ms, and its builder thread builds only the last
  of a burst of parameter changes (3 ms quiet). The old presets, as
  descriptions, are prewarmed when FMOD registers the plug-in.

**Upgrading to v2.4.** Studio will not build a bank whose ProcAudio
instruments lack a parameter the plug-in has ("not compatible with their
placed instances": the event plays silent), and it does not add the new
parameter itself. `procaudio_studio_menu.js` adds Fit Length (off, with
the dial's mapping curve) to every older instrument whenever a project
loads, and says so in the Console; save the project to keep it. Install
the plug-in and the script together.

**Built and tested (2026-11-07, v2.4).** Built against the FMOD Engine
2.02.23 SDK. The DLL exports `FMODGetDSPDescription`.

`plugins/fmod/fmod_host_test.cpp` loads it into the real FMOD engine and
drives it the way Studio does: a new instance per Play, parameters set right
after creation. It renders through FMOD's NRT WAV writer, pacing the cold
pairs at real time. `tests/fmod_host_check.py` checks the render. All pass:

| segment | result |
|---|---|
| impact, steel ball 6 cm | sounds in the first mix block; a one-shot (tail −47 dB) |
| impact retrigger | a new Speed value fires again (Impact mode) |
| drop, cardboard box on concrete | landing + rocking settle; one-shot |
| drop, steel ball | bounce series; one-shot |
| roll | no hit; onset +0.5 dB re steady state |
| slide, steel box on concrete | onset −0.4 dB (no hit) |
| after stop | digital silence |
| held stick → wooden ball (cold, no preset) | sounds 40 ms after Play |
| glass marble → same marble (cold) | sounds 40 ms after Play; the two clicks do not cancel |
| Render: Target only (concrete slab) | the slab alone |
| Size changed while ringing | identical to the unchanged ring |
| held ball + Roll | slides; the Speed dial reads `a held ball can't turn` |
| 1 m steel stick on concrete (long ring) | rings the full 6 s, no cut (v2.2 cut it at 3 s) |
| render capture (`PROCAUDIO_CAPTURE=1`) | the last Play's WAV equals FMOD's render (+0.00 dB) |
| Fit Length 0.15 s, steel-ball hit | identical to the unfitted hit before the fade (−584 dB); last 2 ms −74 dB re peak; silent after |
| Fit Length 0.4 s, roll | silent after 0.4 s |

**Build:**

```powershell
cmake -S cpp -B cpp/build "-DFMOD_SDK_DIR=C:/Program Files (x86)/FMOD SoundSystem/FMOD Studio API Windows"
cmake --build cpp/build --config Release
& "$env:USERPROFILE\miniconda3\envs\procaudio\python.exe" cpp/tests/fmod_host_check.py
node cpp/tests/rules_js_check.js   # the panel's motion rules = the plug-in's (1,224 cases)
```

**Motion rules** (`pa::check_action`; design doc Part 11 has the full
table):
- **Impact** needs something moving (a Free or Held object or target).
- **Drop:** a Free object onto a support. A support is a flat Box, Panel
  or Ground that holds still: on the ground, Built-in, Resting, or Held
  level like a tray.
- **Roll:** a Free or Resting ball on a support.
- **Slide:** a body that does not turn, pressed by a hand or by gravity. A
  free ball rolls instead of sliding.

`cpp/build/Release/procaudio_render.exe rules` dumps every combination as
CSV.

The output is `cpp/build/plugins/fmod/Release/procaudio_fmod.dll`.

**Install:**
- **FMOD Studio 2.02:** close Studio (it locks the DLL), copy
  `procaudio_fmod.dll`, `procaudio_fmod.plugin.js` and
  `procaudio_fmod_divider.png` (all in the build output folder) into
  `%localappdata%\FMOD Studio\Plugins`, and `procaudio_studio_menu.js`
  into `%localappdata%\FMOD Studio\Scripts`, then start Studio. **Building
  does not install**: Studio keeps loading whatever copy is in those folders.
- **The ProcAudio menu** (`procaudio_studio_menu.js`; design doc Parts 8
  and 9). Its items are under Studio's **Scripts** menu, Scripts ›
  ProcAudio › …. How an instrument ends (Studio cuts a synchronous
  instrument dead at its end; its own fade-out and AHDSR release still end
  in a cut on short instruments):
  - **Ending: Fit Selected to Their Length:** the sound fades to silence
    at the instrument's end. Writes each instrument's length into Fit
    Length and makes it synchronous.
  - **Ending: Ring Out Selected (Async + Release):** the sound rings past
    the end: Async + Cut + a volume release (seconds), Fit Length off.
  - **Ending: Update Fitted Lengths (Whole Project):** a script cannot see
    an instrument being resized; run this after resizing fitted ones.
  - **Open Panel (follows the selection):** a window beside the deck that
    adapts, which the deck cannot. It follows the selected ProcAudio
    instrument and hides rows that do not apply. It offers only the
    motions the pair can do and says why. The Shape, Mount and Target
    lists adapt to the other body: a choice under which nothing could
    happen is left out (a Built-in object leaves the target only Free or
    Held). Every change goes straight to
    the instrument (the deck follows), and deck edits show up in the panel
    within 250 ms. It shows how loud these exact settings were when last
    played: **Make audible** raises Level so a quiet pair peaks at
    −12 dBFS. Its Fit to length and Ring out buttons set the ending.
  - **Show Last Render:** imports the newest WAV the plug-in saved
    (`%localappdata%\FMOD Studio\ProcAudio`) and shows its waveform in the
    Assets browser.
- **Plug-in folders.** `%localappdata%\FMOD Studio\Plugins` is
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
