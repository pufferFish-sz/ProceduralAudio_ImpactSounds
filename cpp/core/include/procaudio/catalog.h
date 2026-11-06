// Preset objects and surfaces (the plugin's Object / Surface lists) and a
// process-wide, thread-safe asset cache.
//
// Building a voice from scratch takes ~0.5-1 s (surface tiles: FFTs +
// circle dilation; impact-damping calibration). Every FMOD instrument
// instance is created at Play, so without the cache every Play waited that
// long in silence. Tables are read-only and shared: roll tables per (ball,
// surface), slide tables per surface (independent of the slider), mu per
// (object, surface, fs). With a warm cache a voice is ready in milliseconds.
#pragma once

#include <atomic>
#include <memory>

#include "procaudio/body_desc.h"
#include "procaudio/voice.h"

namespace pa {

int num_object_presets();
const char* object_preset_name(int i);    // "Wood ball S", ...
bool object_preset_is_ball(int i);
int num_surface_presets();
const char* surface_preset_name(int i);   // "Varnished wood", ...
const char* surface_preset_slug(int i);   // SURFACE_PROFILES key

// The sounding body of a preset (boxes include their face-contact damping).
Body make_preset_object(int i);

// A ready-to-play voice: bodies, roll (balls) and slide exciters, impact
// dissipation. Missing assets are built (and cached) in this call.
std::unique_ptr<ContactVoice> make_preset_voice(int object, int surface, double fs);
// True if make_preset_voice() would not have to build anything.
bool preset_assets_ready(int object, int surface, double fs);
// Build every combination's assets, stopping early if *stop becomes true.
void prewarm_presets(double fs, const std::atomic<bool>* stop = nullptr);

// ---------------------------------------------------------- any pair
// Voices for any two described bodies (body_desc.h), with the same
// process-wide cache. Cached per pair: the contact damping (pair_mu). Per
// ball material x radius step x target texture: the rolling tables. Per
// target texture: the sliding tables. Bodies themselves are rebuilt each
// time (analytic, ~ms).

// The old presets as descriptions (Object / Surface lists of plugin v1.2).
BodyDesc preset_object_desc(int i);
BodyDesc preset_surface_desc(int i);

// Radius the rolling tables use: the ball radius on a 1/6-octave grid from
// 1 cm (12 % steps), so a turning Size dial reuses tables. The ball's modes
// keep the exact size.
double roll_radius_step(double radius);

// motion = false builds an impact / drop voice only (no roll or slide
// tables: those take ~0.5 s cold, an impact pair a few ms).
std::unique_ptr<ContactVoice> make_desc_voice(const BodyDesc& object, const BodyDesc& target,
                                              double fs, bool motion = true);
bool desc_assets_ready(const BodyDesc& object, const BodyDesc& target, double fs,
                       bool motion = true);
// Every preset object x preset surface as descriptions (the plugin's start).
void prewarm_desc_presets(double fs, const std::atomic<bool>* stop = nullptr);

}  // namespace pa
