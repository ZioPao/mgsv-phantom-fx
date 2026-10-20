#pragma once

#include <string>

// The engine draws velocity for characters only while its motion blur is on, and DLSS needs that
// velocity but not the blur, which would only smear DLSS's output. Two hooks on the motion blur
// plugin (docs/DEVELOPMENT.md, "Velocity without motion blur") let the add-on ask for one without
// the other, whatever the player chose in the game's settings.
namespace phantom_fx::motion_blur
{
// Hooks the plugin on the supported build, with MinHook initialised; returns what was done, for
// the log. Without the hooks the game's own motion blur setting stays in charge.
std::string hook() noexcept;
// While on, the engine draws its velocity passes every frame and skips the blur; off gives both
// back to the game's setting.
void setVelocityOnly(bool on) noexcept;
}
