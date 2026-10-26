#pragma once

#include <string>

// The engine draws velocity for characters only while its motion blur is on, and DLSS needs that
// velocity whatever the player chose, so the add-on forces the velocity passes on. The blur itself
// is left to the game: with the player's motion blur on, its own blur draws as it always does, and
// with it off neither draws. docs/DEVELOPMENT.md, "Velocity without motion blur".
namespace phantom_fx::motion_blur
{
// Hooks the plugin on the supported build, with MinHook initialised; returns what was done, for
// the log. Without the hooks the game's own motion blur setting stays in charge.
std::string hook() noexcept;
// While on, the engine draws its velocity passes every frame; its blur follows the game's setting.
void setVelocityOnly(bool on) noexcept;
// Off keeps the engine's blur out even with the upscaler on; the strength multiplies the radius the
// game's setting asks for, 1 being the game's own.
void configure(bool enabled, float strength) noexcept;
}
