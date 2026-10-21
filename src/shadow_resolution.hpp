#pragma once

#include <string>

// The sun's shadow map is one atlas of four cascades, 8192x8192 at the game's Extra High shadows.
// A hook on the engine's shadow settings (docs/DEVELOPMENT.md, "Shadow resolution") lets the add-on
// double it, so every cascade gets texels half as wide.
namespace phantom_fx::shadow_resolution
{
// Hooks the settings on the supported build, with MinHook initialised; returns what was done, for
// the log. Without the hook the game's shadow quality stays in charge.
std::string hook() noexcept;
// While on, the sun's shadow map has twice the width and height the game's shadow quality asks for.
void setDoubled(bool on) noexcept;
}
