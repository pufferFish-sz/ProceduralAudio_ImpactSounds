# The same balls dropped on grass and on concrete

Each ball is dropped 1 m onto the lawn and onto a 15 cm concrete slab (scenes/common.bounce_scene; baseball restitution 0.55 on concrete, the MLB ball specification). One shared gain.

All files in this folder share one gain: louder here = louder in the physics.
Stems in `stems/` are each sound source on its own (same gain).

## `drop_baseball-r37mm_from-1m_onto_grass.wav`

**What you hear:** baseball dropped 1 m onto grass

- **Mover:** baseball, radius 3.7 cm, 145 g (cork/yarn core, leather cover: soft, heavily damped). Audible modes: 2558, 3833, 4936, 5163 Hz.
- drop 1 m above the grass tips -> 4.43 m/s at the canopy, 4.53 m/s at the soil; 4 landing(s)
- canopy (5 cm, pushes back 30 Pa) took 7.7 mJ of 1422 mJ
- soil contact: Hertz sphere, k = 5.24e+05 N/m^1.5; first contact 4.0 ms, peak force 428 N (301 g), restitution 0.19 (target 0.3)
- rustle: acoustic energy 0.3 nJ
- level at 1 m: peak 94 dB SPL, LAFmax 68 dB(A), LAeq 57 dB(A); LAeq per stem (dB(A)): baseball-r37mm 51, turf-thud 56, grass-rustle 10
- Energy share: baseball-r37mm 9 %, turf-thud 91 %, grass-rustle 0 %
- Note: two raised seam crossings per revolution (0.12 mm, 6 mm wide) + 30 um lumpiness
- Note: stitches: 12 ridges (40 um, 1.5 mm) around each seam crossing where the track runs along the stitched seam -- count set to the recording's ~57 clicks/s
- Note: pebbled leather: 10 um rms surface texture

## `drop_baseball-r37mm_from-1m_onto_concrete-slab.wav`

**What you hear:** the same baseball dropped 1 m onto a concrete slab

- **Mover:** baseball, radius 3.7 cm, 145 g (cork/yarn core, leather cover: soft, heavily damped). Audible modes: 2558, 3833, 4936, 5163 Hz.
- **Target:** concrete slab on ground, 4 x 4 m, 15 cm thick, installed loss 0.05. Lowest modes: 31, 77, 77, 123 Hz (475 modes used).
- drop height 1 m -> first impact 4.43 m/s; 6 bounces over 1.49 s
- contact stiffness k = 3.19e+07 N/m^1.5 (Hertz, {'E': 100000000.0, 'rho': 706.0511045076713, 'nu': 0.45, 'tan_phi': 0.1} on concrete); first contact 1.24 ms
- restitution 0.55 on the first bounce (calibrated, mu * v = 1.324), 0.53 on the last
- impulse check: max error 0.00 % vs m (v_in + v_out)
- level at 1 m: peak 108 dB SPL, LAFmax 82 dB(A), LAeq 70 dB(A); LAeq per stem (dB(A)): baseball-r37mm 70, concrete-slab 58
- Energy share: baseball-r37mm 67 %, concrete-slab 33 %
- Note: two raised seam crossings per revolution (0.12 mm, 6 mm wide) + 30 um lumpiness
- Note: stitches: 12 ridges (40 um, 1.5 mm) around each seam crossing where the track runs along the stitched seam -- count set to the recording's ~57 clicks/s
- Note: pebbled leather: 10 um rms surface texture
- Note: simply supported plate; radiates per Maidanik, coincidence 120 Hz (inefficient below)

## `drop_wooden-ball-large-r50mm_from-1m_onto_grass.wav`

**What you hear:** solid wooden ball dropped 1 m onto grass

- **Mover:** solid wooden ball, radius 5.0 cm, 340 g, turned hardwood. Audible modes: none audible (all modes > 16 kHz).
- drop 1 m above the grass tips -> 4.43 m/s at the canopy, 4.53 m/s at the soil; 4 landing(s)
- canopy (5 cm, pushes back 30 Pa) took 13.9 mJ of 3339 mJ
- soil contact: Hertz sphere, k = 6.21e+05 N/m^1.5; first contact 5.2 ms, peak force 768 N (230 g), restitution 0.19 (target 0.3)
- rustle: acoustic energy 0.6 nJ
- level at 1 m: peak 97 dB SPL, LAFmax 70 dB(A), LAeq 59 dB(A); LAeq per stem (dB(A)): wooden-ball-large-r50mm 49, turf-thud 59, grass-rustle 13
- Energy share: wooden-ball-large-r50mm 5 %, turf-thud 95 %, grass-rustle 0 %
- Note: no audible modes (lowest sphere mode above 16 kHz): you hear its rigid click and the floor

## `drop_wooden-ball-large-r50mm_from-1m_onto_concrete-slab.wav`

**What you hear:** the same solid wooden ball dropped 1 m onto a concrete slab

- **Mover:** solid wooden ball, radius 5.0 cm, 340 g, turned hardwood. Audible modes: none audible (all modes > 16 kHz).
- **Target:** concrete slab on ground, 4 x 4 m, 15 cm thick, installed loss 0.05. Lowest modes: 31, 77, 77, 123 Hz (475 modes used).
- drop height 1 m -> first impact 4.43 m/s; 7 bounces over 1.75 s
- contact stiffness k = 3.16e+08 N/m^1.5 (Hertz, wood on concrete); first contact 0.67 ms
- restitution 0.60 on the first bounce (calibrated, mu * v = 0.964), 0.60 on the last
- impulse check: max error 0.00 % vs m (v_in + v_out)
- level at 1 m: peak 117 dB SPL, LAFmax 91 dB(A), LAeq 79 dB(A); LAeq per stem (dB(A)): wooden-ball-large-r50mm 79, concrete-slab 68
- Energy share: wooden-ball-large-r50mm 68 %, concrete-slab 32 %
- Note: no audible modes (lowest sphere mode above 16 kHz): you hear its rigid click and the floor
- Note: simply supported plate; radiates per Maidanik, coincidence 120 Hz (inefficient below)
