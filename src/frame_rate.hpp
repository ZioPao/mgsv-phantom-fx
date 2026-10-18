#pragma once

#include <string>

// The game's main loop waits for the next 60 fps tick unless framerate_control in
// TPP_GRAPHICS_CONFIG says "Variable". A hook on the engine's frame wait setter (docs/DEVELOPMENT.md,
// "Frame rate") lets the add-on choose the Variable wait without touching that file.
namespace mgsv_dlss::frame_rate
{
// Hooks the setter on the supported build, with MinHook initialised; returns what was done, for the
// log. Without the hook the game's own setting stays in charge.
std::string hook() noexcept;
// While on, the main loop runs as with "Variable"; off gives the wait back to the game's setting.
void setUnlocked(bool on) noexcept;
}
