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

}  // namespace pa
