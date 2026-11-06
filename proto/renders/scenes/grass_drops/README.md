# Objects dropped onto grass

Balls and empty boxes dropped onto a 5 cm mown lawn (core/grass.py). Thud = the dent pushed into the turf; rustle = blades crushed on the way down; the object's own ring is hardly excited because a soil contact lasts milliseconds. Turf stiffness calibrated to a Clegg hammer reading of 70 g.

All files in this folder share one gain: louder here = louder in the physics.
Stems in `stems/` are each sound source on its own (same gain).

## `drop_baseball-r37mm_from-1m_onto_grass.wav`

**What you hear:** a baseball (145 g) dropped from 1 m onto a mown lawn: thud, short swish, a few small bounces

- **Mover:** baseball, radius 3.7 cm, 145 g (cork/yarn core, leather cover: soft, heavily damped). Audible modes: 2558, 3833, 4936, 5163 Hz.
- drop 1 m above the grass tips -> 4.43 m/s at the canopy, 4.53 m/s at the soil; 4 landing(s)
- canopy (5 cm, pushes back 30 Pa) took 7.7 mJ of 1422 mJ
- soil contact: Hertz sphere, k = 5.24e+05 N/m^1.5; first contact 4.0 ms, peak force 428 N (301 g), restitution 0.19 (target 0.3)
- rustle: acoustic energy 0.3 nJ
- level at 1 m: peak 94 dB SPL, LAFmax 68 dB(A), LAeq 57 dB(A); LAeq per stem (dB(A)): baseball-r37mm 51, turf-thud 56, grass-rustle 10
- texture: 200.5 clicks/s, centroid 153 Hz, >1 kHz kurtosis 32.3, energy below 300 Hz 60 %
- no grass recording yet for this case (recordings/README.md, 'Grass')
- Energy share: baseball-r37mm 9 %, turf-thud 91 %, grass-rustle 0 %
- Note: two raised seam crossings per revolution (0.12 mm, 6 mm wide) + 30 um lumpiness
- Note: stitches: 12 ridges (40 um, 1.5 mm) around each seam crossing where the track runs along the stitched seam -- count set to the recording's ~57 clicks/s
- Note: pebbled leather: 10 um rms surface texture

## `drop_wooden-ball-large-r50mm_from-1m_onto_grass.wav`

**What you hear:** a solid wooden ball (340 g) dropped from 1 m onto a mown lawn: thud, short swish, a few small bounces

- **Mover:** solid wooden ball, radius 5.0 cm, 340 g, turned hardwood. Audible modes: none audible (all modes > 16 kHz).
- drop 1 m above the grass tips -> 4.43 m/s at the canopy, 4.53 m/s at the soil; 4 landing(s)
- canopy (5 cm, pushes back 30 Pa) took 13.9 mJ of 3339 mJ
- soil contact: Hertz sphere, k = 6.21e+05 N/m^1.5; first contact 5.2 ms, peak force 768 N (230 g), restitution 0.19 (target 0.3)
- rustle: acoustic energy 0.6 nJ
- level at 1 m: peak 97 dB SPL, LAFmax 70 dB(A), LAeq 59 dB(A); LAeq per stem (dB(A)): wooden-ball-large-r50mm 49, turf-thud 59, grass-rustle 13
- texture: 125.3 clicks/s, centroid 163 Hz, >1 kHz kurtosis 98.9, energy below 300 Hz 84 %
- no grass recording yet for this case (recordings/README.md, 'Grass')
- Energy share: wooden-ball-large-r50mm 5 %, turf-thud 95 %, grass-rustle 0 %
- Note: no audible modes (lowest sphere mode above 16 kHz): you hear its rigid click and the floor

## `drop_cardboard-box-30x20x20cm_from-0.5m_onto_grass.wav`

**What you hear:** a empty cardboard box (179 g) dropped from 0.5 m onto a mown lawn: thud, short swish, a few small bounces

- **Mover:** cardboard box 30 x 20 x 20cm, 4 mm walls, 0.18 kg, empty, landing on / sliding on its bottom face. Audible modes: 239, 828, 1564, 2006 Hz.
- drop 0.5 m above the grass tips -> 3.13 m/s at the canopy, 3.14 m/s at the soil; 6 landing(s)
- canopy (5 cm, pushes back 30 Pa) took 92.1 mJ of 879 mJ
- soil contact: flat face, k = 5.76e+05 N/m when fully down, lands tilted: contact grows over 17 mm; first contact 2.7 ms, peak force 349 N (199 g), restitution 0.00 (target 0.2)
- rustle: acoustic energy 3.0 nJ
- level at 1 m: peak 122 dB SPL, LAFmax 96 dB(A), LAeq 85 dB(A); LAeq per stem (dB(A)): cardboard-box-30x20x20cm 85, turf-thud 66, grass-rustle 26
- texture: 100.2 clicks/s, centroid 235 Hz, >1 kHz kurtosis 64.1, energy below 300 Hz 96 %
- no grass recording yet for this case (recordings/README.md, 'Grass')
- Energy share: cardboard-box-30x20x20cm 100 %, turf-thud 0 %, grass-rustle 0 %
- Note: bottom panel modes (simply supported, glued and taped loss 0.05); coincidence 4439 Hz
- Note: face load: uniform over the bottom, 81% of it bends the panel (the rest moves the panel's own mass)
- Note: contact damping: loss factor +0.1 while in contact (face lying on the ground)

## `drop_plywood-box-40x30x25cm_from-0.5m_onto_grass.wav`

**What you hear:** a empty plywood crate (4602 g) dropped from 0.5 m onto a mown lawn: thud, short swish, a few small bounces

- **Mover:** plywood box 40 x 30 x 25cm, 12 mm walls, 4.60 kg, empty, landing on / sliding on its bottom face. Audible modes: 407, 1581, 2493, 3666 Hz.
- drop 0.5 m above the grass tips -> 3.13 m/s at the canopy, 3.27 m/s at the soil; 4 landing(s)
- canopy (5 cm, pushes back 30 Pa) took 256.5 mJ of 22573 mJ
- soil contact: flat face, k = 8.15e+05 N/m when fully down, lands tilted: contact grows over 26 mm; first contact 6.9 ms, peak force 5951 N (132 g), restitution 0.21 (target 0.2)
- rustle: acoustic energy 7.6 nJ
- level at 1 m: peak 119 dB SPL, LAFmax 94 dB(A), LAeq 83 dB(A); LAeq per stem (dB(A)): plywood-box-40x30x25cm 82, turf-thud 74, grass-rustle 30
- texture: 72.9 clicks/s, centroid 169 Hz, >1 kHz kurtosis 43.1, energy below 300 Hz 52 %
- no grass recording yet for this case (recordings/README.md, 'Grass')
- Energy share: plywood-box-40x30x25cm 67 %, turf-thud 33 %, grass-rustle 0 %
- Note: bottom panel modes (simply supported, glued loss 0.03); coincidence 1253 Hz
- Note: face load: uniform over the bottom, 80% of it bends the panel (the rest moves the panel's own mass)
- Note: contact damping: loss factor +0.1 while in contact (face lying on the ground)
