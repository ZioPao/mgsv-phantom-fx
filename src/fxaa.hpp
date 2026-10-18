#pragma once

#include <string>

// DLSS takes the place of the game's FXAA pass, so it needs that pass to run, but the game drops it
// when the player turns post processing down. A hook on the FXAA plugin (docs/DEVELOPMENT.md,
// "FXAA with post processing down") lets the add-on keep the pass, whatever the game's setting.
namespace mgsv_dlss::fxaa
{
// Hooks the plugin on the supported build, with MinHook initialised; returns what was done, for
// the log. Without the hook the game's post processing setting stays in charge.
std::string hook() noexcept;
// While on, the engine draws its FXAA pass every frame it has a view for; off gives the pass back
// to the game's setting.
void setForced(bool on) noexcept;
}
