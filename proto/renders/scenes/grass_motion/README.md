# Rolling, sliding and walking on grass

Balls rolled, boxes pulled and a person walking on a 5 cm mown lawn. The rustle's power is eta x the mechanical power lost in the grass (rolling resistance, friction, crushed blades), so heavier and faster is louder by physics.

All files in this folder share one gain: louder here = louder in the physics.
Stems in `stems/` are each sound source on its own (same gain).

## `roll_baseball-r37mm_from-3mps_on_grass.wav`

**What you hear:** a baseball rolled across a mown lawn from 3 m/s until it stops

- **Mover:** baseball, radius 3.7 cm, 145 g (cork/yarn core, leather cover: soft, heavily damped). Audible modes: 2558, 3833, 4936, 5163 Hz.
- rolls from 3 m/s, stops after 1.43 s / 2.14 m (rolling resistance c_rr 0.3)
- sweeps a 7.3 cm strip of blades: 3294 blades/s at the start; loss per blade 0.39 mJ (crushing one blade flat: 0.10 mJ)
- soil contact under the ball: static sink 0.19 mm, contact resonance 44 Hz, airborne 23.8% of the time, 18 landings/s on the lumpy lawn soil, 0.5 mm rms (estimate)
- rustle: acoustic energy 18.7 nJ
- level at 1 m: peak 66 dB SPL, LAFmax 43 dB(A), LAeq 35 dB(A); LAeq per stem (dB(A)): baseball-r37mm 33, grass-rustle 32
- texture: 15.3 clicks/s, centroid 2935 Hz, >1 kHz kurtosis 35.3, energy below 300 Hz 25 %
- no grass recording yet for this case (recordings/README.md, 'Grass')
- Energy share: baseball-r37mm 69 %, grass-rustle 31 %
- Note: two raised seam crossings per revolution (0.12 mm, 6 mm wide) + 30 um lumpiness
- Note: stitches: 12 ridges (40 um, 1.5 mm) around each seam crossing where the track runs along the stitched seam -- count set to the recording's ~57 clicks/s
- Note: pebbled leather: 10 um rms surface texture

## `roll_wooden-ball-large-r50mm_from-3mps_on_grass.wav`

**What you hear:** a solid wooden ball rolled across a mown lawn from 3 m/s until it stops

- **Mover:** solid wooden ball, radius 5.0 cm, 340 g, turned hardwood. Audible modes: none audible (all modes > 16 kHz).
- rolls from 3 m/s, stops after 1.43 s / 2.14 m (rolling resistance c_rr 0.3)
- sweeps a 10.0 cm strip of blades: 4500 blades/s at the start; loss per blade 0.67 mJ (crushing one blade flat: 0.10 mJ)
- soil contact under the ball: static sink 0.31 mm, contact resonance 35 Hz, airborne 18.5% of the time, 15 landings/s on the lumpy lawn soil, 0.5 mm rms (estimate)
- rustle: acoustic energy 43.9 nJ
- level at 1 m: peak 67 dB SPL, LAFmax 43 dB(A), LAeq 37 dB(A); LAeq per stem (dB(A)): wooden-ball-large-r50mm 31, grass-rustle 35
- texture: 6.6 clicks/s, centroid 3099 Hz, >1 kHz kurtosis 9.8, energy below 300 Hz 19 %
- no grass recording yet for this case (recordings/README.md, 'Grass')
- Energy share: wooden-ball-large-r50mm 46 %, grass-rustle 54 %
- Note: no audible modes (lowest sphere mode above 16 kHz): you hear its rigid click and the floor

## `slide_cardboard-box-30x20x20cm_pulled_on_grass.wav`

**What you hear:** an empty cardboard box 30 x 20 x 20cm pulled across a mown lawn in one long drag

- **Mover:** cardboard box 30 x 20 x 20cm, 4 mm walls, 0.18 kg, empty, landing on / sliding on its bottom face. Audible modes: 239, 828, 1564, 2006 Hz.
- pulled 8 s at ~0.50 m/s (scenes/drag.steady_pull); friction mu_k 0.35 -> 0.31 W lost in the grass
- face 600 cm^2 presses 29 Pa (canopy holds 30 Pa): rides on the blades; 149050 blade slip events/s
- rustle: acoustic energy 47.4 nJ
- not modelled yet: the box's own panel driven by the blade flicks (only the rustle is heard)
- level at 1 m: peak 49 dB SPL, LAFmax 34 dB(A), LAeq 30 dB(A); LAeq per stem (dB(A)): grass-rustle 30
- texture: 0.0 clicks/s, centroid 3762 Hz, >1 kHz kurtosis 5.6, energy below 300 Hz 0 %
- no grass recording yet for this case (recordings/README.md, 'Grass')
- Energy share: grass-rustle 100 %
- Note: bottom panel modes (simply supported, glued and taped loss 0.05); coincidence 4439 Hz
- Note: face load: uniform over the bottom, 81% of it bends the panel (the rest moves the panel's own mass)
- Note: contact damping: loss factor +0.1 while in contact (face lying on the ground)

## `slide_plywood-box-40x30x25cm_pulled_on_grass.wav`

**What you hear:** an empty plywood box 40 x 30 x 25cm pulled across a mown lawn in one long drag

- **Mover:** plywood box 40 x 30 x 25cm, 12 mm walls, 4.60 kg, empty, landing on / sliding on its bottom face. Audible modes: 407, 1581, 2493, 3666 Hz.
- pulled 8 s at ~0.50 m/s (scenes/drag.steady_pull); friction mu_k 0.35 -> 7.85 W lost in the grass
- face 1200 cm^2 presses 376 Pa (canopy holds 30 Pa): crushes the blades to the soil; 298101 blade slip events/s
- rustle: acoustic energy 1216.2 nJ
- not modelled yet: the box's own panel driven by the blade flicks (only the rustle is heard)
- level at 1 m: peak 64 dB SPL, LAFmax 48 dB(A), LAeq 44 dB(A); LAeq per stem (dB(A)): grass-rustle 44
- texture: 2.4 clicks/s, centroid 3934 Hz, >1 kHz kurtosis 6.3, energy below 300 Hz 0 %
- no grass recording yet for this case (recordings/README.md, 'Grass')
- Energy share: grass-rustle 100 %
- Note: bottom panel modes (simply supported, glued loss 0.03); coincidence 1253 Hz
- Note: face load: uniform over the bottom, 80% of it bends the panel (the rest moves the panel's own mass)
- Note: contact damping: loss factor +0.1 while in contact (face lying on the ground)

## `walk_4-steps_every-0.55s_on_lawn.wav`

**What you hear:** four footsteps on a mown lawn (dryness 0.3: share of crushed blades that snap)

- walker 75 kg, 4 steps 0.55 s apart; heel strike 5 kg at 0.4 m/s on a foam heel: peak 916 N
- per step the sole crushes 375 blades (30 mJ), the toe slips 1 mm (155 mJ); load 2.0 N per blade -> 30 % of blade events snap (dryness 0.3)
- rustle: acoustic energy 18.6 nJ
- foot/shoe click and the swing of the leg through the grass are not modelled
- level at 1 m: peak 78 dB SPL, LAFmax 44 dB(A), LAeq 39 dB(A) over 0.3-2.5 s; LAeq per stem (dB(A)): turf-thud 37, grass-rustle 33
- texture: 4.6 clicks/s, centroid 48 Hz, >1 kHz kurtosis 57.5, energy below 300 Hz 100 %
- no grass recording yet for this case (recordings/README.md, 'Grass')
- Energy share: turf-thud 100 %, grass-rustle 0 %

## `walk_4-steps_every-0.55s_on_lawn-lush.wav`

**What you hear:** four footsteps on a mown lawn (dryness 0.05: share of crushed blades that snap)

- walker 75 kg, 4 steps 0.55 s apart; heel strike 5 kg at 0.4 m/s on a foam heel: peak 916 N
- per step the sole crushes 375 blades (30 mJ), the toe slips 1 mm (155 mJ); load 2.0 N per blade -> 5 % of blade events snap (dryness 0.05)
- rustle: acoustic energy 18.6 nJ
- foot/shoe click and the swing of the leg through the grass are not modelled
- level at 1 m: peak 78 dB SPL, LAFmax 44 dB(A), LAeq 38 dB(A) over 0.3-2.5 s; LAeq per stem (dB(A)): turf-thud 37, grass-rustle 33
- texture: 6.8 clicks/s, centroid 44 Hz, >1 kHz kurtosis 43.8, energy below 300 Hz 100 %
- no grass recording yet for this case (recordings/README.md, 'Grass')
- Energy share: turf-thud 100 %, grass-rustle 0 %

## `walk_4-steps_every-0.55s_on_lawn-dry.wav`

**What you hear:** four footsteps on a mown lawn (dryness 0.8: share of crushed blades that snap)

- walker 75 kg, 4 steps 0.55 s apart; heel strike 5 kg at 0.4 m/s on a foam heel: peak 916 N
- per step the sole crushes 375 blades (30 mJ), the toe slips 1 mm (155 mJ); load 2.0 N per blade -> 80 % of blade events snap (dryness 0.8)
- rustle: acoustic energy 18.6 nJ
- foot/shoe click and the swing of the leg through the grass are not modelled
- level at 1 m: peak 78 dB SPL, LAFmax 44 dB(A), LAeq 38 dB(A) over 0.3-2.5 s; LAeq per stem (dB(A)): turf-thud 37, grass-rustle 29
- texture: 18.2 clicks/s, centroid 43 Hz, >1 kHz kurtosis 307.7, energy below 300 Hz 100 %
- no grass recording yet for this case (recordings/README.md, 'Grass')
- Energy share: turf-thud 100 %, grass-rustle 0 %
