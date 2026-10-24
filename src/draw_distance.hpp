#pragma once

#include <string>

// How far the game draws and loads its world (docs/DEVELOPMENT.md, "Draw distance"). Fox Engine loads
// a location in square windows of blocks around the camera: full detail (props, vegetation and their
// collision) in 5 x 5 blocks of 128 m, and two coarser copies of the world in 5 x 5 blocks of 256 m
// and 512 m; past those only the terrain. Of what is loaded, a model or a plant is drawn out to a
// distance that grows with its size and the level of detail, plus a length the graphics settings set.
//
// The add-on can widen the windows, which takes effect when a location loads, and raise the level of
// detail and those lengths, which takes effect at once.
namespace phantom_fx::draw_distance
{
struct Options
{
    // Window widths in blocks, odd: full detail, then the two coarser copies; 0 keeps the game's 5.
    // Full detail fails past 7: loading 9 x 9 overruns a fixed table in the engine.
    int fullDetail{}, coarse{}, coarsest{};
    // A level of detail that does not drop with the render size, and longer lengths for models and
    // vegetation, whatever the game's Model Detail.
    bool farDetail{};
};

// Hooks the stream controller's creation on the supported build, with MinHook initialised, and checks
// the level of detail's globals; returns what was done, for the log. Without the hook the windows stay
// the game's.
std::string hook() noexcept;
void configure(const Options& options) noexcept;
// On the game thread at each frame's start: applies the level of detail and lengths, or gives the
// game's back.
void onFrameStart() noexcept;
// Empty while all can apply; otherwise why not, for the overlay.
std::string problem();
// Whether the location in play loaded with other windows than the ones wanted now.
bool waitsForLocation() noexcept;
}
