# ProceduralAudioDemo (Unity 6000.3)

A first-person room for trying the procedural contact sounds (`cpp/`) in a game
scene. The physics drives the C++ model directly. There are no samples and no
FMOD.

Open `Assets/Scenes/ImpactDemo.unity` and press Play. The first Play after
opening the Editor builds about 300 voices on worker threads, which takes
~20 s. The HUD shows "Building voices… N left" until it finishes. The tables
stay cached in the Editor process, so later Play sessions are faster.

## Controls

| | |
|---|---|
| WASD, Shift, Space | move, run, jump |
| Left click | pick up / let go |
| Right click | throw (while holding) |
| Mouse wheel | hold distance |
| Aim at the floor or table while holding | the object is pressed onto it; move the mouse to slide it |
| Walk into a box | push it across the floor |
| 1–7 or the dropdown | ground surface: concrete, wood, varnished wood, tiles, carpet, stone, steel deck |
| R | reset all objects |
| Tab / Esc | free the cursor for the panel; click the scene to look again |

The panel sliders set the Impact / Roll / Slide / Master levels. The defaults
are the FMOD plug-in's presets (0 / +35 / +50 dB): rolling and sliding are
physically much quieter than impacts. The bottom-left readout shows what the
object under the crosshair is doing (rolling 0.84 m/s on Rug, last hit …).
The top-right log lists recent impacts.

## The scene

- **Floor**: switchable, as above. It changes the sound model, the look and the
  physics material.
- **Fixed surfaces**:
  - rug (carpet on wood)
  - 8 mm steel plate
  - varnished wooden table
  - varnished ramp for rolling balls
  - concrete walls
- **Objects**:
  - balls: steel, glass, wood, rubber, stone, plastic
  - tin box
  - wooden box
  - wooden stick
  - plastic crate
  - three cardboard boxes

  Masses come from the model (density × shape).

Rebuild the scene from scratch with **ProcAudio ▸ Build Impact Demo Scene**
(`Assets/ProcAudio/Editor/DemoSceneBuilder.cs`). Rebuilding overwrites the
scene, so either tweak the generated scene directly or change the builder and
rebuild.

## How it works

```
Unity physics ── OnCollisionEnter: normal relative speed ──────────► impact(v)
             └── OnCollisionStay: contact-point tangential speed ──► slide(speed)
                                  centre tangential speed (balls) ─► roll(speed)
                         │
ProcAudioBody (C#) ──P/Invoke──► procaudio_unity.dll ─► pa::ContactVoice (cpp/core)
                         ▲                                     │
OnAudioFilterRead ◄──────┴──── pa_voice_process (audio thread) ◄┘
```

- `ProcAudioBody` (on each rigidbody) holds three native voices per surface:
  - impact
  - free roll/slide
  - held slide (a held ball cannot turn, so it slides)

  It also holds one impact voice per other body. Voices are keyed by the
  surface's `BodyDescription`, so switching the floor builds new voices and
  keeps the old ones. Switching back is instant.
- `ProcAudioSurface` marks static colliders (and their children) as a
  sounding surface. `SurfaceSelector` swaps the floor's description, look and
  physics material.
- Spatialization: each body's 3D `AudioSource` loops a constant-1.0 clip.
  Unity applies distance attenuation (1/r from 1 m) and panning to it, and
  `OnAudioFilterRead` multiplies the procedural signal in. Gains follow the
  plug-in calibration: 0 dB = 63 Pa at 1 m.
- Idle voices go dormant in the native engine (no modal-bank work), so the
  ~300 voices cost nothing until something touches.

## Native plug-in

`Assets/Plugins/x86_64/procaudio_unity.dll` is built from
`cpp/plugins/unity/procaudio_unity.cpp`, which is a C API over the core. To
rebuild it, **close the Editor first** (it locks loaded DLLs), then:

```powershell
cmake --build cpp/build --config Release --target procaudio_unity
```

The post-build step copies the DLL into this project.

## Scripts

`Assets/ProcAudio/Runtime/`:

| file | role |
|---|---|
| `ProcAudioNative.cs` | P/Invoke bindings |
| `BodyDescription.cs` | shape / material / size / mount / finish (mirrors `pa::BodyDesc`) |
| `ProcAudioEngine.cs` | starts the engine, levels, event log, physics settings (10 ms step, low bounce threshold) |
| `ProcAudioBody.cs` | physics → sound, audio rendering |
| `ProcAudioSurface.cs`, `SurfaceSelector.cs`, `SurfaceLook.cs` | surfaces, floor switching, procedural textures |
| `FirstPersonController.cs`, `ObjectGrabber.cs` | player, pushing, grabbing |
| `DemoHUD.cs` | panel, dropdown, sliders, readouts |
