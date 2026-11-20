#pragma once

#include <string>

// The game's main loop, for the devkit's tools: the frame start PhantomFX hooks for Reflex too, with
// only the devkit's calls on it. Another addon's own loop hook would have to call them instead.
namespace devaddon
{
// Hooks the loop on the supported build, with MinHook initialised; returns what was done, for the log.
std::string hook() noexcept;
void unhook() noexcept;
}
