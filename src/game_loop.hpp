#pragma once

#include <cstdint>
#include <string>

// The game loop's frames, for Reflex (frame_generation.hpp) and the weather menu (weather.hpp). Each
// main loop frame starts by stepping game time and counting the frame (0x7F840), updates the game,
// and ends waiting for the next one (0x7FDB0); docs/DEVELOPMENT.md, "Frame generation". Reflex
// sleeps at the start, before the game reads its input, and the frame's markers carry the game's
// count. The weather menu's changes apply at the start, on the game thread.
namespace phantom_fx::game_loop
{
// Hooks the frame start and end on the supported build, with MinHook initialised; returns what was
// done, for the log. Without the hooks the presents mark the simulation (frame_generation.cpp) and
// the weather menu changes nothing.
std::string hook() noexcept;
// The frame the game loop runs now, or last ran; 0 before the hooks saw one.
std::uint32_t currentFrame() noexcept;
}
