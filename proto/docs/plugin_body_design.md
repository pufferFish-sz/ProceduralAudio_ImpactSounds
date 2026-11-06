# Plugin bodies: any object against any object

Design for the next FMOD plugin parameters (started 2026-11-06). The plugin
should cover any two objects meeting: a stick knocking a wooden ball, two
equal glasses clinking, a box dropped on a floor. Part 1 is the design.
Part 2 is the phased plan. Part 3 records what phase 1 built and measured.

---

## Part 1 — Design

### 1.1 The problem with v1.2

The v1.2 plugin (`cpp/plugins/fmod/procaudio_fmod.cpp`) has two lists:

- **Object:** 8 presets that mix shape, material and size in one name
  ("Wood ball S", "Steel box L").
- **Surface:** 5 floors.

That causes three problems.

1. **It only knows "object on floor".** A stick hitting a ball, or two equal
   objects, cannot be described.
2. **The game decides the target, not the designer.** A held object can hit
   anything in the level, and several different things within one second.
   One per-instance Surface setting cannot follow that.
3. **Size has two steps.** Designers want a continuous size and a separate
   shape choice.

Underneath, the C++ core already treats both sides of a contact the same
way: `ContactVoice` holds two `Body`s. Every hit is two bodies sharing one
contact force, and each rings with its own modes. So most of the change is
in how bodies are described, not in the DSP.

### 1.2 One description for every body

Each side of a contact is a `BodyDesc` (`cpp/core/include/procaudio/body_desc.h`):

| field | meaning |
|---|---|
| Shape | Ball, Stick, Box, Panel, Slab (later: Rod/Pipe, Plate/Disc) |
| Material | metal, glass, wood, plastic, rubber, stone, cardboard, concrete, ceramic |
| Size (m) | the shape's main dimension, see the table below |
| Thickness (m) | 0 = the shape and material default |
| Mount | how the body is held: Free, Held, Resting, Built-in |
| Hit (0..1) | where it is struck: 0 = center, 1 = end or edge |
| Finish | Bare, Varnished, Tiled, Carpeted: the contact material and roll/slide texture (added in phase 2, so tile and rug floors are descriptions too) |

What the Size knob means:

| Shape | Size | Thickness | Modes from |
|---|---|---|---|
| Ball | diameter | — (solid) | Lamb sphere (`sphere_modes`) |
| Stick | length | square cross-section, default length / 25 | free-free beam (`bar_modes`), hit point exact |
| Box | longest side (others 2/3 of it) | wall | bottom-panel plate (`box_body`) |
| Panel | longer span (other = half) | plate thickness | simply supported plate (`plate_ss_modes`) |
| Slab | span (square) | slab thickness | plate modes below 6 kHz + forced radiation (§1.5) |

Size follows physics automatically. For a solid shape whose thickness scales
with it, doubling the size halves every pitch and multiplies the mass by 8
(geometric similarity). For a box with a fixed wall, pitch falls 4× and mass
rises 4×. Phase 1 tests check both rules.

### 1.3 Mount: how the body is held

Mount replaces the old idea of a "surface". A floor is a slab or panel whose
Mount is Built-in.

| Mount | Everyday meaning | What the model does |
|---|---|---|
| **Free** | thrown, falling, hanging | rings fully, recoils, makes the rigid-body click |
| **Held** | in a hand | ring damped by the grip (loss + 0.02). A held **stick** is a rigid striker pivoting at the hand: no modes, tip mass M/3 (this follows `proto/core/bodies.wood_stick`: a free-free stick's bending modes would take most of the swing's energy) |
| **Resting** | sitting on a table or floor, hit from above | the support carries it: fixed in the contact, no click, contact damping + 0.10 |
| **Built-in** | floor, wall, anchored | fixed, no click; the plate uses its installed loss factor |

### 1.4 Pair physics: no N × N matrix

How two bodies behave together is calculated from the two descriptions, so
N bodies need N descriptions, not N² presets:

| pair property | rule | cost |
|---|---|---|
| hardness k | Hertz from both materials and both contact radii (`impact_stiffness`) | instant |
| effective mass | the mover's strike mass against a fixed body; else m₁m₂/(m₁+m₂) (`pair_effective_mass`) | instant |
| strike mass | Body::strike_mass: a free stick hit off-center feels M/(1 + 3h²), so M/4 at its end; a held stick's tip feels M/3 | instant |
| restitution | the lossier material wins: e = min(e_a, e_b) (`pair_restitution`) | instant |
| contact damping μ | `pair_mu`: see §3.2 | 1–7 short contact solves, done at build time |
| who rings | each body's own modes, in one coupled contact solve | per body |
| roll / slide textures | per target surface and object radius | phase 2 (cache by size step) |

Two equal free objects ("same as object") need nothing special: each has
effective mass M/2, both ring with the same modes, and each makes a click.

### 1.5 Floors: slab vs panel

My earlier guess was that a concrete slab adds almost nothing, so it could
drop to a few heavily damped modes. **The A/B test disproved this** (§3.3).
For small, hard strikers (steel balls), most of the sound comes from the
slab: the full plate's floor stem is up to 11 dB louder than the ball. This
agrees with Qu & James's ground-sound work: the ground is a real sound
source, not a backdrop. A slab keeps its modes up to 6 kHz (280 of 475),
which stays within 1 dB of the full plate for every catalog object.

The next step for slabs is a **hybrid modal + statistical** model. Above
about 2 kHz the slab's modes overlap (bandwidth ηf about equals the mode
spacing), so their sum is smooth and could become a fixed filter on the
contact force (infinite-plate mobility 1/(8√(D ρh)), radiation efficiency
about 1 above coincidence). That would keep about 90 modes plus a few
biquads. It is a research item, not a phase 1 task.

Panels (wood floor bays, decks, table tops, walls) stay modal. They really
do ring, and their modes are what make a wooden floor "thump".

### 1.6 FMOD deck layout

```
 OBJECT (this event)           TARGET (what it hits)           MOTION
 Shape     [Stick      ▾]      Source    [From game  ▾]        Motion  [Impact ▾]
 Material  [Wood       ▾]      Shape     [Ball       ▾]        Impact  (2.0 m/s)
 Size      ( 60 cm )           Material  [Wood       ▾]        Speed   (0.5 m/s)
 Thickness ( 2.4 cm )          Size      ( 12 cm )             Render  [Both ▾]
 Mount     [Held       ▾]      Thickness ( — )                 Level   (0 dB)
 Hit       ( 1.0 )             Mount     [Free       ▾]
                               Hit       ( 0.3 )
```

- **Target** has three options:
  - **Set here:** use the dials, for auditioning in Studio.
  - **Same as object:** two equal bodies.
  - **From game:** the dials are only defaults, and each hit carries its
    real target (§1.7). This one comes with the body registry in phase 4:
    v2.0 offers only the first two.
- **Render: Both / Object only / Target only.** If the target has its own
  emitter (a bell that should keep ringing, a shared floor), each event
  renders only its own body, so nothing sounds twice.
- **Size dial:**
  - **Logarithmic:** FMOD's piecewise-linear float mapping, so going from
    1 to 2 cm feels the same as 50 cm to 1 m (both halve the pitch).
  - **Shows what it means:** the plugin writes its own value label
    (`getParameterFloat` → `valuestr`), for example `12 cm · 0.61 kg · 1.9 kHz`.
- **Changes apply at the next contact.** A ringing object never jumps in
  pitch. During roll or slide the change crossfades.
- **Defaults reproduce the old presets:** Ball / metal / 60 mm is exactly
  "Steel ball L", and Box / cardboard / 30 cm is "Card box S" (tested).

### 1.7 Game side

FMOD parameters belong to an event instance, not to a hit, but a held
object meets many targets. So the target travels with each hit:

1. **`SoundBody` component (Unity) on each collider:** Shape, Material and
   Mount. **Size comes from the collider's bounds** (sphere radius, box
   extents), so designers rarely type a size. Engine physics materials map
   to sound materials.
2. **Register each body once:** the game sends its description and gets an
   id (a small registry inside the plugin).
3. **Each hit sends a `ContactMsg`** with the target id, speed and sample
   offset. The struct already has `object_id` and `surface_id` fields;
   `surface_id` becomes `target_id`.

---

## Part 2 — Phases

| phase | content | status |
|---|---|---|
| 1 | core: `BodyDesc`, `make_body`, Mount, strike / effective mass, pair restitution + `pair_mu`, slab model with A/B, tests | **done** (§3) |
| 2 | plugin v2.0: three-column deck, Target (Set here / Same as object), Finish, Render, log Size dial with value label, apply-at-next-contact, roll/slide tables cached by size step for any target | **done** (Part 4) |
| 3 | port Rod/Pipe (`tube`) and Plate/Disc (`disc`) from Python, with real disc mode shapes (Bessel) so Hit gives rim vs center | |
| 4 | game side: body registry over Contacts ("From game" target, several targets per voice), Unity `SoundBody` with size from colliders, physics-material map | |
| research | hybrid modal + statistical slab (§1.5); a grip model so held sticks can ring | |

---

## Part 3 — Phase 1 results (2026-11-06)

Code: `cpp/core/include/procaudio/body_desc.h`, `cpp/core/src/body_desc.cpp`.
Small changes in `bodies.h` (`Body::strike_mass`) and `voice.h/.cpp` (the
pair effective mass is used for every impact, and `set_restitution` sets
drop sequences). Tests are in `cpp/tests/test_core.cpp`: `test_body_desc`,
`test_pair_restitution`, `test_pair_voices`, `test_slab_ab`.

### 3.1 Bodies

- **Presets are special cases.** Ball / metal / 60 mm gives exactly
  "Steel ball L". Box / cardboard / 30 cm (+ face damping) gives "Card box S".
  A built-in wood panel 2.4 m × 22 mm gives `floor_body("wood-floor")`.
- **Size scaling:** Ball and Stick, 20 → 40 cm: f₁ halves, mass × 8.
  Box (fixed wall): f₁ ÷ 4, mass × 4.
- **Every combination builds:** all 540 shape × material × mount × size
  (min, 0.3 m, max) bodies have positive mass and underdamped modes.
- **Effective masses:** two equal free balls → M/2; ball on slab → M;
  held stick tip → M/3 with no modes.

### 3.2 Restitution and contact damping

The analytic Flores et al. (2011) damping alone,
λ = 8(1 − e)k / (5 e v), **misses by up to 0.63** in restitution. It only
counts losses inside the contact, while light floors and thin box walls
take energy into their own vibration. `pair_mu` fixes this:

1. One solve without contact damping measures what the modes take (e_modes).
2. Set the contact's share to e_target / e_modes and refine with up to 6
   solves, keeping the best result.

Results over the 8 preset objects × 5 floors:

| class | pairs | result |
|---|---|---|
| reachable | 23 | within 0.022 of the target |
| `*` the modes alone take more than the target | 5 | e.g. steel ball L on a wood floor rebounds 0.32, target 0.60: the light floor absorbs the rest. Physically plausible. |
| `^` contact damping cannot bring it down | 12 | cardboard boxes rebound 0.9 against a target of 0.3. Their thin walls are the springy part, so the contact barely compresses and its damping does nothing. The catalog path (`calibrate_mu`) has the same limit. Fix: a real cardboard loss model (crushing), later. |

### 3.3 Slab A/B

Each catalog object hits at 2 m/s; the mix (object + floor) is compared
against the full 4 × 4 m, 15 cm concrete plate (475 modes). The table gives
the worst deviation in any 1/3-octave band within 30 dB of the loudest:

| slab modes below | modes | worst deviation |
|---|---|---|
| none (forced radiation only) | 0 | 25.1 dB |
| 2 kHz | 91 | 5.3 dB |
| 4 kHz | 184 | 3.1 dB |
| **6 kHz (chosen)** | **280** | **0.85 dB** |

The deviation comes from the steel balls. The floor is 11 dB above the
large ball's own sound and 2 dB below the small ball's, which has no
audible modes. For wood balls and boxes, a 2 kHz cutoff would already be
within 1 dB.

### 3.4 Not done in phase 1

- No plugin or Studio UI changes. The plugin still uses the catalog presets.
- `make_pair_voice` covers impacts and drops; roll and slide for arbitrary
  pairs need the size-stepped texture cache (phase 2).
- Approximations to state in the thesis:
  - a free panel uses simply supported plate modes;
  - sticks have a square cross-section (round rods arrive with `tube`);
  - boxes ring with their bottom panel only;
  - Resting treats the support as rigid;
  - restitution presets and the min rule are plausibility values, not
    measurements.

---

## Part 4 — Phase 2 results: plugin v2.0 (2026-11-06)

Code:
- `cpp/plugins/fmod/procaudio_fmod.cpp` and `.plugin.js`: the deck.
- `cpp/core/include/procaudio/catalog.h`: `make_desc_voice`,
  `desc_assets_ready`, `prewarm_desc_presets`.
- `body_desc.h`: Finish.
- `voice.cpp`: the free-target delay.

The parameter table is in `cpp/README.md`.

### 4.1 What the designer sees

- **Three deck columns:** OBJECT, TARGET, MOTION.
  - Object and target each have Shape, Material, Finish and Mount
    dropdowns, plus Size, Thickness and Hit dials.
  - Motion has the Motion and Render dropdowns, plus Impact, Speed and
    Level dials.
- **Size and Thickness are logarithmic** (FMOD piecewise-linear mapping,
  0.5 cm – 20 m and 0 – 30 cm).
- **Their labels show what they mean** (read back through FMOD in the host
  test): `60 cm, 225 g, no ring` for a held stick (a rigid striker),
  `auto (2.4 cm)` for its thickness.
- **Context-menu examples:** stick knocks a wooden ball, two glass
  marbles clink, cardboard box dropped on concrete, steel ball rolls on a
  wood floor.

### 4.2 Behaviour and how it was checked (real FMOD engine)

| check | result |
|---|---|
| v1.2 segments (impact, retrigger, drops, roll, slide, stop) | all pass, same levels as v1.2 |
| held stick → free wooden ball, no preset, played at real-time pace | sound 40 ms after Play |
| glass marble → same marble | sound 40 ms after Play |
| Render: Target only | the slab alone |
| object Size changed while the hit rings | render identical to the same hit without the change (−573 dB) |
| C++ unit tests / golden tests vs Python | 582 / 18 pass |

### 4.3 Problems found and fixed on the way

1. **Two equal free bodies were silent.** Their rigid-body clicks have
   opposite signs (one body is pushed each way). Radiated from one point,
   they cancel exactly; two marbles with no audible modes made no sound at
   all. Each body radiates from its own center, so the target's stem now
   arrives later by (center separation) / (c √3). The √3 averages the
   listener direction. Low frequencies still cancel, as they physically do
   for two close dipoles. `Body::center_offset` holds the distance. Fixed
   targets (floors) have no click and are unchanged.
2. **Cold pairs took ~0.6–1 s.** Studio sets a Play's parameters one at a
   time. Each setter rebuilt the dial labels (a sphere mode solve), so the
   builder started building stale in-between settings. The fixes:
   - labels are computed only when Studio reads them;
   - the builder waits for 3 ms without a new request;
   - roll/slide tables (~0.5 s cold) are built only when Motion needs them.
3. **The host test's clock.** FMOD's NRT writer renders ~50× faster than
   real time, so a builder thread's few milliseconds looked like a second
   of audio. The cold-pair segments are now paced at real time.

### 4.4 Open points

- **A solid 12 cm wooden ball has no audible ring in this model.** Its
  lowest mode is ~17.6 kHz, so "stick knocks a wooden ball" is clicks only.
  Real wooden balls are anisotropic and often hollow or turned with
  defects. A hollow-shell sphere (later shape) would ring.
- **Not yet tried inside FMOD Studio:** the deck layout and the dial value
  labels were checked through the engine API only. Studio's deck height
  and whether it shows plug-in value strings still need a look.
- **"From game" target and several targets per voice:** phase 4.

---

## Part 5 — v2.1 deck clean-up after the first look in Studio (2026-11-06)

| change | why |
|---|---|
| Finish values: **Bare, Polished, Tiled, Carpeted** (Varnished → Polished) | "Polished" covers any smooth coat (varnish, lacquer, polish), not just wood |
| **Finish only on the target** ("Surface finish") | It describes the surface the object hits, lands, rolls or slides on. On the object it meant wrapping a thrown cube in carpet or tiles, and "Polished" on an object changed nothing at all. `BodyDesc::finish` stays in the core (the object's is always Bare). A soft striker (padded mallet) would be its own option later. |
| **Hit position** (was Hit point; parameters `Obj Hit Pos` / `Tgt Hit Pos`) | Its label now says it in words: `center`, `30% to the edge`, `edge`; sticks `middle` … `end` (held: `tip`); balls `any spot (ball)`, because a sphere sounds the same wherever it is hit |
| **One Speed knob** (Impact removed) | Impact and Speed were never used together: Impact/Drop read the hit speed, Roll/Slide the rolling speed. One knob, with a label that says which: `hit at 2 m/s`, `drop 2 m/s = from 20 cm` (v² = 2gh), `rolling at 0.5 m/s`. Log dial 0–10 m/s. Game code still sends both speeds per contact through `ContactMsg` (v_normal, v_tangent), so a ball that bounces while rolling is not limited by the knob. |
| Framed columns and a line between OBJECT and TARGET | `isFramed` layouts plus a 1 px line image (`procaudio_fmod_divider.png`, a Pixmap widget; Studio 2.02 has no separator widget) |

All C++ unit tests (582), golden tests and the FMOD host check pass
unchanged. The deck script was checked against the DLL's parameter names
(18/18 bound, every preset key valid).

### Grass: what exists in Python, and why it is not ported yet

`proto/core/aggregate.py` has a grass preset (v3, layered PhISEM):
- swish grains, 10–60 ms with soft onsets;
- sparse sub-ms crackles;
- a thin noise bed.

It replaced v2, which sounded like steady hiss. But it is **not ready to
become a Finish**:

1. **Never checked against a recording.** Every value is an ear-tuned
   heuristic (formulas_explained.md: "project heuristic"), and
   `proto/recordings/` has no grass at all.
2. **Not driven by contact physics.** A hand-drawn 0–1 footstep envelope
   drives it, and each layer is peak-normalized, so it has no physical
   level. A ball dropped on grass and a person stepping on it would come
   out equally loud.
3. **Grass is more than a rustle under an object.** It is a soft, lossy
   contact (blades and soil yield, little bounce) plus the rustle plus the
   ground underneath. It needs a contact model (closer to Carpeted than to
   a hard floor) and a rustle driven by contact force and speed.

Plan for a Grass finish (its own piece of work):
- record references: a ball and a box dropped on grass, rolled and slid on
  it, footsteps;
- drive the aggregate layer from the contact (impact energy, normal force ×
  slip speed) with physical levels;
- give grass a soft-soil contact material;
- compare with the recordings;
- then port to C++.

---

## Part 6 — v2.2: Target dropdown, Ground, floor-only finish (2026-11-06)

| change | why |
|---|---|
| **"Same as object" is the first choice of the Target dropdown** (Same as object, Ball, Stick, Box, Panel, Ground); the separate "Set here / Same as object" switch and `Tgt Shape` are gone | choosing the target is now one decision. With Same as object the other target controls are ignored, and the target dials read `same as object`. |
| **Slab → Ground** (`Shape::Ground`) | the shape is a floor on the earth (concrete, stone; grass is being modelled separately), not an abstract slab. Wooden floors stay Panels (a floor bay on joists). |
| **Finish applies to floors only** (`floor_finish()`: Panel and Ground), displayed as "Floor finish" | a tiled or carpeted ball is not a thing; a floor's finish is what the object hits, lands, rolls or slides on |

**Why nothing greys out.** The user asked for the ignored controls to be
greyed out and unselectable. FMOD Studio 2.02's plug-in deck
(`registerPluginDescription` → `deckUi`) has no enabled or visible state
and no change callbacks: the deck is built once from a static description.
The manual's `isEnabled` / `isVisible` / `onValueChanged` belong to Studio's
scripted dialogs (`studio.ui.showModalDialog`), not to plug-in decks. The
plug-in cannot push values back into Studio's controls either. So:
- impossible combinations are removed from the choices where possible;
- ignored controls say so in their value labels;
- the finish is named for where it applies.

A scripted dialog (a "ProcAudio body editor" menu command) could grey
things out. It is a possible later addition if the deck is still
confusing.

Tests: 584 C++ unit tests (two new: a finish on a ball is ignored, a tiled
Ground is ceramic), golden tests unchanged, and the FMOD host check passes,
including a new check that the target's Size label reads `same as object`
in that mode.

---

## Part 7 — Grass: a physically driven model, in Python (2026-11-06)

Part 5 found that the old grass (v3 rustle in `core/aggregate.py`) could
not become a Finish. It was driven by a hand-drawn 0–1 envelope, so it had
no real loudness, and grass had no contact model. This part replaces it
with a model driven by contact physics: `core/grass.py`, a physical drive
for the grain layer (`aggregate.grain_stream_pa`), and listening scenes in
`scenes/grass.py`. The C++ and the FMOD plugin are unchanged.

### 7.1 What grass is, in three layers

| layer | what it does | model and numbers |
|---|---|---|
| **Blades (canopy)** | ~15 000 blades per m², 5 cm tall. Each is a thin strip (3 × 0.3 mm). It bends over at only ~2 mN (Euler buckling), so the whole lawn pushes back with only **30 Pa**. | An empty cardboard box (30 Pa) rests on the blades; a ball (300 Pa) or a foot goes through to the soil. Crushing one blade costs ~0.1 mJ. That lost energy is what we hear as **rustle**. |
| **Turf/soil** | Soft and lossy. Things sink in, leave a dent, and barely bounce. | Hertz law for balls, flat-face law for boxes and heels. Stiffness is set so that this model's drop of a **Clegg hammer** (the standard turf tester: 2.25 kg, 0.45 m) reads **70 g**, a typical kept lawn (US football fields must stay below 100 g). That gives E ≈ 1.9 MPa (`materials.PHYSICAL["turf"]`). Energy loss is **plastic**: the soil keeps a dent and gives back only e² of the work (e = 0.3 ball, 0.2 flat face). |
| **Ground sound ("thud")** | A body sinking into its dent pushes air away, like a slow loudspeaker set into the ground. | Air flow = (body's cross-section at ground level) × speed; pressure ρ₀ · d(flow)/dt / (2πr). The soil around the dent also moves; that is not counted, so this is a lower bound. |

Why grass sounds "muffled": a soil contact lasts **4–7 ms** (a hard floor:
0.1–0.5 ms). The force then has almost nothing above a few hundred Hz, so
the object's own ring is barely excited. You hear the low thud of the
dent, not the object.

### 7.2 What drives the rustle (the main change)

Rustle **sound power = η × the mechanical power lost in the grass**:

| motion | power lost in the grass | blade events per second |
|---|---|---|
| drop / step | canopy push-back × speed, plus the blades' kinetic energy | blades newly pressed per second |
| roll | rolling resistance × weight × speed (c_rr = 0.3) | blades in the ball's path |
| slide | friction × weight × speed (μ = 0.35) | one event per 3 mm of slip per blade under the face |
| step toe-off | friction × load × slip speed (1 mm slip) | as slide |

- **η is one number for every motion.** That is the main assumption. It is
  set from one published level: Fuda et al. 2015 measured **28.5 dB(A)**
  walking on park grass with a dummy head, which is about 32 dB(A) at 1 m.
  That gives **η = 2.2 × 10⁻⁸**. Their number includes park background, so
  it is an upper bound.
- **Snaps (crackle)** take a share of the power. The share grows with
  the load per blade (1 − e^(−F/0.2 N)) times "dryness". A foot puts ~2 N
  on each blade, so it snaps stems; a ball puts ~0.02 N, so it hardly does.
  Presets: `lawn` (dryness 0.3), `lawn-lush` (0.05), `lawn-dry` (0.8).
- **Grains** keep the v3 sounds (swish 0.7–6 kHz, 10–60 ms; crackle
  2.5–9 kHz, sub-ms). These spectra are still tuned by ear. Each grain
  now carries its share of the acoustic energy, in Pa at 1 m. At most
  200 swish and 150 crackle grains per second are used; above that,
  each grain carries several blades' energy, so the total power stays
  exact.

Two independent estimates agree. Rolling resistance puts **0.3 mJ** into
each blade the baseball pushes aside. Crushing a blade flat costs **0.1 mJ**.
These are the same order, which is what a single η needs.

### 7.3 Results (`renders/scenes/grass_*`, all at 1 m)

| scene | LAFmax dB(A) | LAeq dB(A) | loudest part |
|---|---|---|---|
| baseball (145 g) dropped 1 m on grass | 68 | 57 | thud 56, ball 51, rustle 10 |
| same baseball on concrete | 82 | 70 | ball's click |
| solid wooden ball (340 g) 1 m on grass / on concrete | 70 / 91 | 59 / 79 | thud / ball |
| empty cardboard box (0.18 kg) 0.5 m on grass | 96 | 85 | box panel ⚠ see 7.5 |
| empty plywood crate (4.6 kg) 0.5 m on grass | 94 | 83 | crate panel 82, thud 74 |
| baseball rolled at 3 m/s (stops after 2.1 m) | 43 | 35 | ball 33, rustle 32 |
| wooden ball rolled at 3 m/s | 43 | 37 | rustle 35 |
| cardboard box pulled at 0.5 m/s | 34 | 30 | rustle |
| plywood crate pulled at 0.5 m/s | 48 | 44 | rustle (+14 dB: 26× the weight) |
| 4 steps, 75 kg walker | 44 | 39 | thud 37, rustle 33 |

What this shows:
- **Grass vs concrete: 14–21 dB quieter**, with the same balls and the
  same drop. That is the same size of gap Fuda et al. measured between
  walking on grass and on wood (28.5 vs 48.6 dB(A)), though it is a
  different situation.
- **Heavy vs light is physics now.** The crate drags 14 dB louder than
  the cardboard box. A ball's drop is a thud with almost no rustle (the
  canopy takes only 0.5 % of the energy). A light box rides on the blades.
- **Dryness** changes the texture, not the level. Snapped stems on the dry
  lawn: ~18 clicks/s in the 4-step walk, vs ~5 on the normal lawn.

### 7.4 Problems found and fixed on the way

1. **Viscous soil made a force spike.** For a barely bouncing box,
   Hunt–Crossley damping must be huge (μv ≈ 5–10). The force then jumps at
   first touch, and that spike rang the cardboard box at 104 dB(A). Soil
   is plastic, not sticky, so it now uses a load/unload law that leaves a
   dent (`drop_on_grass`).
2. **Perfectly flat landings** stopped a 0.18 kg box in 0.6 ms at
   ~1000 g. Real boxes land a few degrees tilted, so the contact area now
   grows from one edge over side × sin 5° of sinking.
3. **The thud jumped** when a ball left its dent, because only the
   contact patch counted. Now the cross-section at ground level counts,
   which is continuous: air keeps flowing while the ball climbs out.
4. **A box's panel was hit at one point.** A face landing is pushed all
   over, so it now uses uniform-load coupling (`shapes.plate_ss(load=
   "uniform")`). The panel's own inertia takes part of the load, and the
   face gets contact damping from the ground. The panel modes are also
   **solved inside the landing**. The cardboard box's bottom is 3× softer
   than the turf, so it cushions its own landing (bounce 0, peak 199 g
   instead of 406 g).
5. **Grain energy fell short.** Snap amplitudes are heavy-tailed, so a
   typical render of a few hundred grains came out 1.5–5 dB short.
   Amplitudes are now clipped at 2σ and normalized; over 200 renders the
   energy averages 1.04× the target. A short walk still varies ±2 dB from
   seed to seed.

### 7.5 Not checked yet / open

- **No grass recording exists.** Spectra, event density and the
  rustle/thud balance are all unchecked. The recording plan is in
  `recordings/README.md` ("Grass"). Its key trick: drop the same ball on
  the lawn **and on a patio** with the same phone. The model already
  predicts the patio drop in Pa, so that recording calibrates the phone,
  and then the lawn drop measures grass in real units (and fixes η).
  `scenes/grass.py` compares against any file it finds there,
  automatically.
- **The heel thud is too loud.** Alone it is 37 dB(A) at 1 m, above
  Fuda's whole-walk total. Either the heel model (5 kg at 0.4 m/s on a
  foam heel, peak 916 N) is too hard, or their microphone was farther
  away. Check this first with `walk_lawn_1m.wav`.
- **The cardboard box drop (96 dB(A)) is the least trusted render.** An
  empty box is a drum. The model still loads its bottom panel as soon as
  it touches, but a tilted box first lands on an edge, where the walls
  take the load and the panel hardly bends. Expect the real thing to be
  quieter.
- **Balls bounce less than intended: e = 0.19 instead of 0.3.** The
  light damping that lets bodies come to rest also eats some rebound.
  Restitution could be targeted after damping.
- **The rolling ball hops** over the guessed soil lumps (0.5 mm rms,
  airborne 24 % of the time). That gives it its own sound, as loud as the
  rustle. The blades probably damp this. Measure, or lower the lumps.
- **A sliding box's own panel is silent.** The blade flicks should drive
  it; not modelled.
- **Unmeasured guesses:** c_rr = 0.3 for a 5 cm lawn (golf greens of
  3 mm give ~0.08 from the Stimpmeter), μ = 0.35, blade E = 0.3 GPa, toe
  slip 1 mm, the grain spectra.

### 7.6 Plan for the C++ port (`Finish::Grass`)

Port after the recordings, not before. Pieces, in order:

1. **`Finish::Grass`** in `body_desc.h` (floors only: Panel and Ground,
   like the other finishes). Contact material `turf` (E 1.9 MPa); target
   restitution 0.3 / 0.2 instead of the soft-surface 0.10 in
   `material_restitution`.
2. **Soil law in the impact solver** (`impact.cpp`): add the elasto-plastic
   load/unload branch next to Hunt–Crossley. Add the canopy as a
   constant-pressure force (30 Pa × footprint) before the soil. Keep
   restitution per landing (the voice already re-scales μ per hit). Use
   a dent depth state per voice for repeated bounces.
3. **Thud**: one extra output per contact, ρ₀ d(A_section · v)/dt / 2πr,
   from the solver's depth and speed. It is cheap: one value per sample
   while in contact.
4. **Rustle exciter** (new, next to `RollExciter`/`SlideExciter` in
   `exciters.h`): inputs power (W) and event rate per block, from the
   voice's Motion and speed (roll: c_rr N v; slide: μ N v; impact: from
   the solver). The grain pool is capped at 200 + 150 grains/s, so it is
   bounded: about 12 overlapping biquad grains on average. Use a seeded
   RNG, and match the golden test to the Python `grain_stream_pa` by
   energy and spectrum, not sample by sample.
5. **Roll/slide on Grass**: skip the profile exciters' high-frequency
   texture (the soft contact filters it out). Keep the lumpy-soil roll
   only if the recordings show its thumps.
6. **Deck**: Finish gains "Grass"; a later "Dry ↔ Lush" knob maps to
   dryness. The level label stays physical (dB at 1 m), so a ball on
   grass is quieter than on concrete without any designer trim.

Sources for the anchors: ASTM F1702 (Clegg hammer on natural turf) and the
NFL's 100 g limit (Penn State Center for Sports Surface Research,
"Surface hardness — natural turf guide"); Fuda et al., Energy Procedia
2015 (Politecnico di Torino, iris.polito.it: walking recorded with a dummy
head on grass, wood, stone and gravel in urban parks; the measuring
distance still needs checking in the paper).
