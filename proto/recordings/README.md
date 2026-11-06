# Reference recordings

Put real contact-sound recordings here for spectrogram comparison
(phone recordings are fine at this stage). Suggested set:

- wood block tap
- ceramic mug tap
- metal pot tap
- object dropped on floor  (name it `*floor*.wav` — stage4 auto-includes it)
- object dragged on table
- gravel scrape

For each file, note below: device, distance, room.

| file | device | distance | notes |
|------|--------|----------|-------|
|      |        |          |       |

## Library references (added 2026-10-27)

`Rolling/` (9 files) and `Sliding/` (16 files) are library Foley
recordings: close-miked, uncalibrated, recording chain unknown. They are
used for SHAPE comparisons only (spectrum shape, texture statistics), never
for absolute level. The analysis and the render-vs-real numbers are in
`docs/rolling_sliding_vs_recordings.md`.

Matched to renders:
- `scenes/roll.py`:
  - baseball on hardwood;
  - full soda can on hardwood / on rug;
  - AA battery on tile.
- `scenes/drag.py`:
  - metal box, smooth calm;
  - chess pieces;
  - wood box on cement (stutter).

Notes:
- `Sliding/471747-...-CDAP (1).wav` is a duplicate of `471747-...-CDAP.wav`
  and is skipped by the analysis.
- Still missing: our own references at a KNOWN distance (phone at 1 m is
  fine) for the same objects, e.g. a can and a ball rolled on one floor, and
  a box dragged on it. Without them the low band (< 300 Hz) cannot be
  compared: close miking boosts it.

## Grass (planned, added 2026-11-06)

`scenes/grass.py` models grass from contact physics (`core/grass.py`) and
compares itself with any file in `recordings/Grass/` automatically: a file
is matched to a scene by the words in its name (`drop` + `ball`, `drop` +
`box`, `roll` + `ball`, `slide` + `box`, `walk` or `step`). Nothing is
recorded yet, so the grass model is so far checked only against two
published numbers (a Clegg hammer reading and one walking level).

What to record (a phone is fine; most important first):

| file name (in `Grass/`) | what | why |
|---|---|---|
| `drop_ball_baseball_1m_lawn.wav` and `drop_ball_baseball_1m_patio.wav` | the same ball dropped from 1 m onto the lawn, then onto concrete/paving, **same phone, same spot, same gain** | the concrete drop is already predicted in Pa by the model, so it fixes the phone's absolute level; then the lawn drop measures the grass thud and rustle in real units |
| `walk_lawn_1m.wav` | 6-8 normal steps past the phone, phone at 1 m from the path, 30 cm above the grass | sets the rustle efficiency eta and checks the heel thud |
| `roll_ball_lawn.wav` | a ball (baseball or football) rolled across the lawn past the phone at 1 m, about walking speed | rolling rustle level and spectrum |
| `slide_box_lawn.wav` | an empty cardboard box, then a heavier box or crate, dragged ~2 m past the phone | heavy vs light: the model says +14 dB for the crate |
| `drop_box_lawn.wav` | an empty cardboard box dropped flat-ish from 0.5 m | the least-trusted render (see design doc Part 7) |
| `*_dry` / `*_wet` variants | the same walk on dry and on wet/lush grass, if possible | snapping (crackle) vs swish |

For every file write a row in the table at the top: device, distance,
grass height (cm) and whether it was dry or wet. Note the lawn height:
the model assumes 5 cm. Keep the phone's automatic gain off if the app
allows it (or record the patio drop in the same session, which is the
level reference anyway). Avoid wind: even light wind in leaves is ~20-30
dB(A), about as loud as the rolling ball.
